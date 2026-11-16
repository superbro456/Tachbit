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
static tb_mark tb_temp_begin(void) {
    tb_temp_depth += 1;
    return (tb_mark){tb_nchunks, tb_nchunks > 0 ? tb_chunks[tb_nchunks - 1].used : 0};
}
// длина последних текстов: s[i] в цикле не пересчитывает strlen каждый раз
static _Thread_local const char *tb_lc_p[2];
static _Thread_local long long tb_lc_n[2];
static _Thread_local int tb_lc_next;
static inline void tb_lc_clear(void) { tb_lc_p[0] = 0; tb_lc_p[1] = 0; }
static void tb_temp_end(tb_mark m) {
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
// вырасти кусок: временный растёт во временной памяти, обычный — через realloc
static void *tb_resize(void *p, long long old_n, long long new_n) {
    tb_lc_clear();
    if (tb_nchunks > 0 && tb_in_temp(p)) {
        void *q = tb_temp_alloc(new_n);
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
    int in_temp = tb_nchunks > 0 && tb_in_temp(d->keys->data);
    if (old && !in_temp) free(old);
    void *p = in_temp ? tb_temp_alloc(bytes) : malloc(bytes);
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
static tb_o_Comp *tb_new_Comp(const char * a0, tb_list * a1, long long a2, tb_list * a3, tb_list * a4, tb_list * a5, tb_list * a6, tb_list * a7, const char * a8, tb_list * a9, long long a10, long long a11, long long a12, long long a13, tb_list * a14, long long a15, tb_o_Node * a16, long long a17, long long a18, _Bool a19, _Bool a20, tb_list * a21, long long a22, tb_o_Node * a23, tb_list * a24, tb_list * a25, tb_list * a26, long long a27, tb_list * a28, tb_list * a29, tb_list * a30, long long a31, tb_list * a32, tb_list * a33, tb_list * a34, tb_list * a35, tb_list * a36, tb_list * a37, tb_list * a38, tb_list * a39) {
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
    return (tb_run(({ const char *tb_c17_0 = "test -d "; long long tb_c17_0n = sizeof("test -d ") - 1; const char *tb_c17_1 = f_main__quote(v_path); long long tb_c17_1n = (long long)strlen(tb_c17_1); char *tb_c17r = tb_mem(tb_c17_0n + tb_c17_1n + 1), *tb_c17rw = tb_c17r; memcpy(tb_c17rw, tb_c17_0, tb_c17_0n); tb_c17rw += tb_c17_0n; memcpy(tb_c17rw, tb_c17_1, tb_c17_1n); tb_c17rw += tb_c17_1n; *tb_c17rw = 0; (const char *)tb_c17r; })) == 0LL);
    tb_crash(67, "функция is_folder дошла до конца без return");
    return 0;
}

static _Bool f_main__is_file(const char * v_path) {
    return (tb_run(({ const char *tb_c18_0 = "test -f "; long long tb_c18_0n = sizeof("test -f ") - 1; const char *tb_c18_1 = f_main__quote(v_path); long long tb_c18_1n = (long long)strlen(tb_c18_1); char *tb_c18r = tb_mem(tb_c18_0n + tb_c18_1n + 1), *tb_c18rw = tb_c18r; memcpy(tb_c18rw, tb_c18_0, tb_c18_0n); tb_c18rw += tb_c18_0n; memcpy(tb_c18rw, tb_c18_1, tb_c18_1n); tb_c18rw += tb_c18_1n; *tb_c18rw = 0; (const char *)tb_c18r; })) == 0LL);
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
    tb_o_Comp * v_c = tb_new_Comp(v_path, tb_list_new(sizeof(tb_o_Token *)), 0LL, tb_list_new(sizeof(tb_o_Type *)), tb_list_new(sizeof(tb_o_Func *)), tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Use *)), tb_list_new(sizeof(const char *)), "", tb_list_new(sizeof(tb_o_Var *)), 0LL, 0LL, 0LL, 0LL, tb_list_new(sizeof(long long)), 0LL, 0, 0LL, 0LL, 0, 0, tb_list_new(sizeof(const char *)), 0LL, 0, tb_list_new(sizeof(const char *)), tb_list_new(sizeof(const char *)), tb_list_new(sizeof(const char *)), 0LL, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(const char *)), tb_list_new(sizeof(long long)), 0LL, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(long long)), tb_bits_new(), tb_list_new(sizeof(long long)), tb_list_new(sizeof(long long)), tb_bits_new(), tb_list_new(sizeof(const char *)));
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
}

static _Bool f_lexer__is_command(const char * v_word) {
    tb_list * v_commands = tb_list_from(40, sizeof(const char *), (const char *[]){"func", "return", "gives", "if", "else", "each", "in", "count", "from", "to", "repeat", "times", "while", "skip", "stop", "const", "shared", "obj", "link", "int", "dec", "text", "bool", "and", "or", "not", "true", "false", "use", "modpub", "modpri", "temp", "together", "background", "danger", "by", "list", "dict", "fail", "try"});
    {
        tb_list * tb_l82 = v_commands;
        for (long long tb_i82 = 0; tb_i82 < tb_l82->len; tb_i82++) {
            const char * const v_c = ((const char * *)tb_l82->data)[tb_i82];
            if ((strcmp(v_c, v_word) == 0)) {
                return 1;
            }
        }
    }
    return 0;
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
                if ((f_lexer__at(v_s, v_pos) == 9LL)) {
                    { long long tb_v100 = 4LL; long long *tb_p100 = &(v_spaces); *tb_p100 += tb_v100; }
                } else {
                    { long long tb_v101 = 1LL; long long *tb_p101 = &(v_spaces); *tb_p101 += tb_v101; }
                }
                { long long tb_v102 = 1LL; long long *tb_p102 = &(v_pos); *tb_p102 += tb_v102; }
            }
            if ((((v_pos >= v_n) || (f_lexer__at(v_s, v_pos) == 13LL)) || ((f_lexer__at(v_s, v_pos) == 47LL) && (f_lexer__at(v_s, (v_pos + 1LL)) == 47LL)))) {
                continue;
            }
            long long v_top = (*(long long *)tb_at(v_indents, ((v_indents)->len - 1LL), sizeof(long long), 62));
            if ((v_spaces > v_top)) {
                if (((v_indents)->len == 200LL)) {
                    f_errors__error_at(v_c, v_line, "слишком глубокая вложенность", "");
                }
                { long long tb_v103 = v_spaces; *(long long *)tb_grow(v_indents) = tb_v103; }
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
                    { long long tb_v104 = 1LL; long long *tb_p104 = &(v_pos); *tb_p104 += tb_v104; }
                    continue;
                }
                if (((v_ch == 47LL) && (f_lexer__at(v_s, (v_pos + 1LL)) == 47LL))) {
                    break;
                }
                long long v_start = v_pos;
                if (f_lexer__is_letter(v_ch)) {
                    while ((f_lexer__is_letter(f_lexer__at(v_s, v_pos)) || f_lexer__is_digit(f_lexer__at(v_s, v_pos)))) {
                        { long long tb_v105 = 1LL; long long *tb_p105 = &(v_pos); *tb_p105 += tb_v105; }
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
                        { long long tb_v106 = 1LL; long long *tb_p106 = &(v_pos); *tb_p106 += tb_v106; }
                    }
                    if (((f_lexer__at(v_s, v_pos) == 46LL) && f_lexer__is_digit(f_lexer__at(v_s, (v_pos + 1LL))))) {
                        v_kind = "decimal";
                        { long long tb_v107 = 1LL; long long *tb_p107 = &(v_pos); *tb_p107 += tb_v107; }
                        while (f_lexer__is_digit(f_lexer__at(v_s, v_pos))) {
                            { long long tb_v108 = 1LL; long long *tb_p108 = &(v_pos); *tb_p108 += tb_v108; }
                        }
                    }
                    f_lexer__add_token(v_c, v_kind, tb_cut(v_s, v_start, (v_pos - 1LL), 101), v_line);
                } else if ((v_ch == 34LL)) {
                    { long long tb_v109 = 1LL; long long *tb_p109 = &(v_pos); *tb_p109 += tb_v109; }
                    while (((v_pos < v_n) && (f_lexer__at(v_s, v_pos) != 34LL))) {
                        if ((f_lexer__at(v_s, v_pos) == 92LL)) {
                            { long long tb_v110 = 1LL; long long *tb_p110 = &(v_pos); *tb_p110 += tb_v110; }
                        }
                        { long long tb_v111 = 1LL; long long *tb_p111 = &(v_pos); *tb_p111 += tb_v111; }
                    }
                    if ((f_lexer__at(v_s, v_pos) != 34LL)) {
                        f_errors__error_at(v_c, v_line, "текст в кавычках не закрыт", "");
                    }
                    { long long tb_v112 = 1LL; long long *tb_p112 = &(v_pos); *tb_p112 += tb_v112; }
                    f_lexer__add_token(v_c, "string", tb_cut(v_s, v_start, (v_pos - 1LL), 111), v_line);
                } else {
                    const char * v_a = tb_char_at(v_s, v_pos, -1, 113);
                    const char * v_b = "";
                    if (((v_pos + 1LL) < v_n)) {
                        v_b = tb_char_at(v_s, (v_pos + 1LL), -1, 116);
                    }
                    if (f_lexer__two_char_sign(v_a, v_b)) {
                        f_lexer__add_token(v_c, "sign", ({ const char *tb_c113_0 = v_a; long long tb_c113_0n = (long long)strlen(tb_c113_0); const char *tb_c113_1 = v_b; long long tb_c113_1n = (long long)strlen(tb_c113_1); char *tb_c113r = tb_mem(tb_c113_0n + tb_c113_1n + 1), *tb_c113rw = tb_c113r; memcpy(tb_c113rw, tb_c113_0, tb_c113_0n); tb_c113rw += tb_c113_0n; memcpy(tb_c113rw, tb_c113_1, tb_c113_1n); tb_c113rw += tb_c113_1n; *tb_c113rw = 0; (const char *)tb_c113r; }), v_line);
                        { long long tb_v114 = 2LL; long long *tb_p114 = &(v_pos); *tb_p114 += tb_v114; }
                    } else if (f_lexer__one_char_sign(v_a)) {
                        f_lexer__add_token(v_c, "sign", v_a, v_line);
                        { long long tb_v115 = 1LL; long long *tb_p115 = &(v_pos); *tb_p115 += tb_v115; }
                    } else {
                        f_errors__error_at(v_c, v_line, "непонятный символ", v_a);
                    }
                }
            }
            f_lexer__add_token(v_c, "newline", "", v_line);
        }
    }
    if (((tb_tlen(v_src) > 0LL) && tb_ends(v_src, "\n"))) {
        { long long tb_v116 = 1LL; long long *tb_p116 = &(v_line); *tb_p116 -= tb_v116; }
    }
    while (((v_indents)->len > 1LL)) {
        long long v_gone = (*(long long *)tb_pop(v_indents, 130));
        f_lexer__add_token(v_c, "dedent", "", (v_line + 1LL));
    }
    f_lexer__add_token(v_c, "end", "", (v_line + 1LL));
}

static const char * f_lexer__token_kind_name(const char * v_kind) {
    if (({ const char *tb_s117 = v_kind; ((unsigned char)tb_s117[0] == 110 && (unsigned char)tb_s117[1] == 97 && (unsigned char)tb_s117[2] == 109 && (unsigned char)tb_s117[3] == 101 && tb_s117[4] == 0); })) {
        return "ИМЯ";
    }
    if (({ const char *tb_s118 = v_kind; ((unsigned char)tb_s118[0] == 99 && (unsigned char)tb_s118[1] == 111 && (unsigned char)tb_s118[2] == 109 && (unsigned char)tb_s118[3] == 109 && (unsigned char)tb_s118[4] == 97 && (unsigned char)tb_s118[5] == 110 && (unsigned char)tb_s118[6] == 100 && tb_s118[7] == 0); })) {
        return "КОМАНДА";
    }
    if (({ const char *tb_s119 = v_kind; ((unsigned char)tb_s119[0] == 110 && (unsigned char)tb_s119[1] == 117 && (unsigned char)tb_s119[2] == 109 && (unsigned char)tb_s119[3] == 98 && (unsigned char)tb_s119[4] == 101 && (unsigned char)tb_s119[5] == 114 && tb_s119[6] == 0); })) {
        return "ЧИСЛО";
    }
    if (({ const char *tb_s120 = v_kind; ((unsigned char)tb_s120[0] == 100 && (unsigned char)tb_s120[1] == 101 && (unsigned char)tb_s120[2] == 99 && (unsigned char)tb_s120[3] == 105 && (unsigned char)tb_s120[4] == 109 && (unsigned char)tb_s120[5] == 97 && (unsigned char)tb_s120[6] == 108 && tb_s120[7] == 0); })) {
        return "ДРОБЬ";
    }
    if (({ const char *tb_s121 = v_kind; ((unsigned char)tb_s121[0] == 115 && (unsigned char)tb_s121[1] == 116 && (unsigned char)tb_s121[2] == 114 && (unsigned char)tb_s121[3] == 105 && (unsigned char)tb_s121[4] == 110 && (unsigned char)tb_s121[5] == 103 && tb_s121[6] == 0); })) {
        return "ТЕКСТ";
    }
    if (({ const char *tb_s122 = v_kind; ((unsigned char)tb_s122[0] == 115 && (unsigned char)tb_s122[1] == 105 && (unsigned char)tb_s122[2] == 103 && (unsigned char)tb_s122[3] == 110 && tb_s122[4] == 0); })) {
        return "ЗНАК";
    }
    if (({ const char *tb_s123 = v_kind; ((unsigned char)tb_s123[0] == 110 && (unsigned char)tb_s123[1] == 101 && (unsigned char)tb_s123[2] == 119 && (unsigned char)tb_s123[3] == 108 && (unsigned char)tb_s123[4] == 105 && (unsigned char)tb_s123[5] == 110 && (unsigned char)tb_s123[6] == 101 && tb_s123[7] == 0); })) {
        return "КОНЕЦ СТРОКИ";
    }
    if (({ const char *tb_s124 = v_kind; ((unsigned char)tb_s124[0] == 105 && (unsigned char)tb_s124[1] == 110 && (unsigned char)tb_s124[2] == 100 && (unsigned char)tb_s124[3] == 101 && (unsigned char)tb_s124[4] == 110 && (unsigned char)tb_s124[5] == 116 && tb_s124[6] == 0); })) {
        return "ОТСТУП";
    }
    if (({ const char *tb_s125 = v_kind; ((unsigned char)tb_s125[0] == 100 && (unsigned char)tb_s125[1] == 101 && (unsigned char)tb_s125[2] == 100 && (unsigned char)tb_s125[3] == 101 && (unsigned char)tb_s125[4] == 110 && (unsigned char)tb_s125[5] == 116 && tb_s125[6] == 0); })) {
        return "ВЫСТУП";
    }
    return "КОНЕЦ ФАЙЛА";
    tb_crash(134, "функция token_kind_name дошла до конца без return");
    return "";
}

static void f_lexer__print_tokens(tb_o_Comp * v_c) {
    long long tb_rl_name = -1, tb_rc_name = 0; (void)tb_rl_name; (void)tb_rc_name;
    tb_list * v_out = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l126 = ((tb_o_Comp *)tb_nn(v_c, 158))->q_tokens;
        for (long long tb_i126 = 0; tb_i126 < tb_l126->len; tb_i126++) {
            tb_o_Token * const v_t = ((tb_o_Token * *)tb_l126->data)[tb_i126];
            const char * v_num = ({ const char *tb_c127_0 = ""; long long tb_c127_0n = sizeof("") - 1; char tb_c127_1d[24]; long long tb_c127_1n; const char *tb_c127_1 = tb_digits(((tb_o_Token *)tb_nn(v_t, 159))->q_line, tb_c127_1d, &tb_c127_1n); char *tb_c127r = tb_mem(tb_c127_0n + tb_c127_1n + 1), *tb_c127rw = tb_c127r; memcpy(tb_c127rw, tb_c127_0, tb_c127_0n); tb_c127rw += tb_c127_0n; memcpy(tb_c127rw, tb_c127_1, tb_c127_1n); tb_c127rw += tb_c127_1n; *tb_c127rw = 0; (const char *)tb_c127r; });
            while ((tb_tlen(v_num) < 3LL)) {
                v_num = ({ const char *tb_c128_0 = " "; long long tb_c128_0n = sizeof(" ") - 1; const char *tb_c128_1 = v_num; long long tb_c128_1n = (long long)strlen(tb_c128_1); char *tb_c128r = tb_mem(tb_c128_0n + tb_c128_1n + 1), *tb_c128rw = tb_c128r; memcpy(tb_c128rw, tb_c128_0, tb_c128_0n); tb_c128rw += tb_c128_0n; memcpy(tb_c128rw, tb_c128_1, tb_c128_1n); tb_c128rw += tb_c128_1n; *tb_c128rw = 0; (const char *)tb_c128r; });
            }
            const char * v_name = f_lexer__token_kind_name(((tb_o_Token *)tb_nn(v_t, 162))->q_kind); tb_rl_name = -1;
            while (((tb_rl_name >= 0 ? tb_rl_name : tb_tlen(v_name)) < 13LL)) {
                tb_app_n(&v_name, &tb_rl_name, &tb_rc_name, " ", sizeof(" ") - 1); 
            }
            { const char * tb_v129 = ({ const char *tb_c130_0 = v_num; long long tb_c130_0n = (long long)strlen(tb_c130_0); const char *tb_c130_1 = ": "; long long tb_c130_1n = sizeof(": ") - 1; const char *tb_c130_2 = v_name; long long tb_c130_2n = (long long)strlen(tb_c130_2); const char *tb_c130_3 = " "; long long tb_c130_3n = sizeof(" ") - 1; const char *tb_c130_4 = ((tb_o_Token *)tb_nn(v_t, 165))->q_word; long long tb_c130_4n = (long long)strlen(tb_c130_4); char *tb_c130r = tb_mem(tb_c130_0n + tb_c130_1n + tb_c130_2n + tb_c130_3n + tb_c130_4n + 1), *tb_c130rw = tb_c130r; memcpy(tb_c130rw, tb_c130_0, tb_c130_0n); tb_c130rw += tb_c130_0n; memcpy(tb_c130rw, tb_c130_1, tb_c130_1n); tb_c130rw += tb_c130_1n; memcpy(tb_c130rw, tb_c130_2, tb_c130_2n); tb_c130rw += tb_c130_2n; memcpy(tb_c130rw, tb_c130_3, tb_c130_3n); tb_c130rw += tb_c130_3n; memcpy(tb_c130rw, tb_c130_4, tb_c130_4n); tb_c130rw += tb_c130_4n; *tb_c130rw = 0; (const char *)tb_c130r; }); *(const char * *)tb_grow(v_out) = tb_v129; }
        }
    }
    (tb_show_4(tb_join(v_out, "\n")), tb_ps("\n"));
}

static tb_o_Node * f_parser__new_node(const char * v_kind, long long v_line) {
    return tb_new_Node(v_kind, v_line, "", "", 0, 0, (-1LL), 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    tb_crash(7, "функция new_node дошла до конца без return");
    return 0;
}

static void f_parser__add_kid(tb_o_Node * v_parent, tb_o_Node * v_kid) {
    { tb_o_Node * tb_v131 = v_kid; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_parent, 11))->q_kids) = tb_v131; }
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
    if (({ const char *tb_s132 = ((tb_o_Token *)tb_nn((*(tb_o_Token * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 27))->q_tokens, ((tb_o_Comp *)tb_nn(v_c, 27))->q_pos, sizeof(tb_o_Token *), 27)), 27))->q_kind; !((unsigned char)tb_s132[0] == 101 && (unsigned char)tb_s132[1] == 110 && (unsigned char)tb_s132[2] == 100 && tb_s132[3] == 0); })) {
        { long long tb_v133 = 1LL; long long *tb_p133 = &(((tb_o_Comp *)tb_nn(v_c, 28))->q_pos); *tb_p133 += tb_v133; }
    }
}

static _Bool f_parser__is_sign(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__now(v_c);
    return (({ const char *tb_s134 = ((tb_o_Token *)tb_nn(v_t, 32))->q_kind; ((unsigned char)tb_s134[0] == 115 && (unsigned char)tb_s134[1] == 105 && (unsigned char)tb_s134[2] == 103 && (unsigned char)tb_s134[3] == 110 && tb_s134[4] == 0); }) && (strcmp(((tb_o_Token *)tb_nn(v_t, 32))->q_word, v_s) == 0));
    tb_crash(30, "функция is_sign дошла до конца без return");
    return 0;
}

static _Bool f_parser__is_cmd(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__now(v_c);
    return (({ const char *tb_s135 = ((tb_o_Token *)tb_nn(v_t, 36))->q_kind; ((unsigned char)tb_s135[0] == 99 && (unsigned char)tb_s135[1] == 111 && (unsigned char)tb_s135[2] == 109 && (unsigned char)tb_s135[3] == 109 && (unsigned char)tb_s135[4] == 97 && (unsigned char)tb_s135[5] == 110 && (unsigned char)tb_s135[6] == 100 && tb_s135[7] == 0); }) && (strcmp(((tb_o_Token *)tb_nn(v_t, 36))->q_word, v_s) == 0));
    tb_crash(34, "функция is_cmd дошла до конца без return");
    return 0;
}

static _Bool f_parser__ahead_is_sign(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__ahead(v_c);
    return (({ const char *tb_s136 = ((tb_o_Token *)tb_nn(v_t, 40))->q_kind; ((unsigned char)tb_s136[0] == 115 && (unsigned char)tb_s136[1] == 105 && (unsigned char)tb_s136[2] == 103 && (unsigned char)tb_s136[3] == 110 && tb_s136[4] == 0); }) && (strcmp(((tb_o_Token *)tb_nn(v_t, 40))->q_word, v_s) == 0));
    tb_crash(38, "функция ahead_is_sign дошла до конца без return");
    return 0;
}

static const char * f_parser__what_is_here(tb_o_Comp * v_c) {
    const char * v_k = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 43))->q_kind;
    if (({ const char *tb_s137 = v_k; ((unsigned char)tb_s137[0] == 110 && (unsigned char)tb_s137[1] == 101 && (unsigned char)tb_s137[2] == 119 && (unsigned char)tb_s137[3] == 108 && (unsigned char)tb_s137[4] == 105 && (unsigned char)tb_s137[5] == 110 && (unsigned char)tb_s137[6] == 101 && tb_s137[7] == 0); })) {
        return "конец строки";
    }
    if (({ const char *tb_s138 = v_k; ((unsigned char)tb_s138[0] == 105 && (unsigned char)tb_s138[1] == 110 && (unsigned char)tb_s138[2] == 100 && (unsigned char)tb_s138[3] == 101 && (unsigned char)tb_s138[4] == 110 && (unsigned char)tb_s138[5] == 116 && tb_s138[6] == 0); })) {
        return "лишний отступ";
    }
    if (({ const char *tb_s139 = v_k; ((unsigned char)tb_s139[0] == 100 && (unsigned char)tb_s139[1] == 101 && (unsigned char)tb_s139[2] == 100 && (unsigned char)tb_s139[3] == 101 && (unsigned char)tb_s139[4] == 110 && (unsigned char)tb_s139[5] == 116 && tb_s139[6] == 0); })) {
        return "конец блока";
    }
    if (({ const char *tb_s140 = v_k; ((unsigned char)tb_s140[0] == 101 && (unsigned char)tb_s140[1] == 110 && (unsigned char)tb_s140[2] == 100 && tb_s140[3] == 0); })) {
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
    if (({ const char *tb_s141 = ((tb_o_Token *)tb_nn(v_t, 66))->q_kind; !((unsigned char)tb_s141[0] == 110 && (unsigned char)tb_s141[1] == 97 && (unsigned char)tb_s141[2] == 109 && (unsigned char)tb_s141[3] == 101 && tb_s141[4] == 0); })) {
        if (({ const char *tb_s142 = ((tb_o_Token *)tb_nn(v_t, 67))->q_kind; ((unsigned char)tb_s142[0] == 99 && (unsigned char)tb_s142[1] == 111 && (unsigned char)tb_s142[2] == 109 && (unsigned char)tb_s142[3] == 109 && (unsigned char)tb_s142[4] == 97 && (unsigned char)tb_s142[5] == 110 && (unsigned char)tb_s142[6] == 100 && tb_s142[7] == 0); })) {
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
    if (({ const char *tb_s143 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 74))->q_kind; !((unsigned char)tb_s143[0] == 110 && (unsigned char)tb_s143[1] == 101 && (unsigned char)tb_s143[2] == 119 && (unsigned char)tb_s143[3] == 108 && (unsigned char)tb_s143[4] == 105 && (unsigned char)tb_s143[5] == 110 && (unsigned char)tb_s143[6] == 101 && tb_s143[7] == 0); })) {
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
        { const char * tb_v144 = "dict"; const char * *tb_p144 = &(((tb_o_Node *)tb_nn(v_t, 86))->q_name); *tb_p144 = tb_v144; }
        f_parser__next(v_c);
        tb_o_Node * v_key = f_parser__parse_type(v_c);
        if ((({ const char *tb_s145 = ((tb_o_Node *)tb_nn(v_key, 89))->q_name; !((unsigned char)tb_s145[0] == 116 && (unsigned char)tb_s145[1] == 101 && (unsigned char)tb_s145[2] == 120 && (unsigned char)tb_s145[3] == 116 && tb_s145[4] == 0); }) && ({ const char *tb_s146 = ((tb_o_Node *)tb_nn(v_key, 89))->q_name; !((unsigned char)tb_s146[0] == 105 && (unsigned char)tb_s146[1] == 110 && (unsigned char)tb_s146[2] == 116 && tb_s146[3] == 0); }))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_key, 90))->q_line, "ключ у dict бывает text или int", ((tb_o_Node *)tb_nn(v_key, 90))->q_name);
        }
        f_parser__add_kid(v_t, v_key);
        if (({ const char *tb_s147 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 92))->q_kind; ((unsigned char)tb_s147[0] == 110 && (unsigned char)tb_s147[1] == 101 && (unsigned char)tb_s147[2] == 119 && (unsigned char)tb_s147[3] == 108 && (unsigned char)tb_s147[4] == 105 && (unsigned char)tb_s147[5] == 110 && (unsigned char)tb_s147[6] == 101 && tb_s147[7] == 0); })) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после ключа напиши тип значения: dict text int", "");
        }
        f_parser__add_kid(v_t, f_parser__parse_type(v_c));
        return v_t;
    }
    if (f_parser__at_type_word(v_c)) {
        { const char * tb_v148 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 97))->q_word; const char * *tb_p148 = &(((tb_o_Node *)tb_nn(v_t, 97))->q_name); *tb_p148 = tb_v148; }
        f_parser__next(v_c);
        if (({ const char *tb_s149 = ((tb_o_Node *)tb_nn(v_t, 99))->q_name; ((unsigned char)tb_s149[0] == 108 && (unsigned char)tb_s149[1] == 105 && (unsigned char)tb_s149[2] == 115 && (unsigned char)tb_s149[3] == 116 && tb_s149[4] == 0); })) {
            if (((!f_parser__at_type_word(v_c)) && ({ const char *tb_s150 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 100))->q_kind; !((unsigned char)tb_s150[0] == 110 && (unsigned char)tb_s150[1] == 97 && (unsigned char)tb_s150[2] == 109 && (unsigned char)tb_s150[3] == 101 && tb_s150[4] == 0); }))) {
                f_errors__error_at(v_c, f_parser__line_now(v_c), "после list напиши, что в нём лежит: list int, list text ...", "");
            }
            f_parser__add_kid(v_t, f_parser__parse_type(v_c));
        }
        return v_t;
    }
    if (({ const char *tb_s151 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 104))->q_kind; ((unsigned char)tb_s151[0] == 110 && (unsigned char)tb_s151[1] == 97 && (unsigned char)tb_s151[2] == 109 && (unsigned char)tb_s151[3] == 101 && tb_s151[4] == 0); })) {
        { const char * tb_v152 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 105))->q_word; const char * *tb_p152 = &(((tb_o_Node *)tb_nn(v_t, 105))->q_name); *tb_p152 = tb_v152; }
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
    { const char * tb_v153 = v_sign; const char * *tb_p153 = &(((tb_o_Node *)tb_nn(v_n, 115))->q_value); *tb_p153 = tb_v153; }
    f_parser__add_kid(v_n, v_left);
    f_parser__add_kid(v_n, v_right);
    return v_n;
    return v_result;
}

static tb_o_Node * f_parser__parse_call(tb_o_Comp * v_c, const char * v_name, long long v_line) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_call = f_parser__new_node("call", v_line);
    { const char * tb_v154 = v_name; const char * *tb_p154 = &(((tb_o_Node *)tb_nn(v_call, 122))->q_name); *tb_p154 = tb_v154; }
    f_parser__expect_sign(v_c, "(");
    while ((!f_parser__is_sign(v_c, ")"))) {
        tb_o_Node * v_arg = f_parser__new_node("arg", f_parser__line_now(v_c));
        if ((({ const char *tb_s155 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 127))->q_kind; ((unsigned char)tb_s155[0] == 110 && (unsigned char)tb_s155[1] == 97 && (unsigned char)tb_s155[2] == 109 && (unsigned char)tb_s155[3] == 101 && tb_s155[4] == 0); }) && f_parser__ahead_is_sign(v_c, ":"))) {
            { const char * tb_v156 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 128))->q_word; const char * *tb_p156 = &(((tb_o_Node *)tb_nn(v_arg, 128))->q_name); *tb_p156 = tb_v156; }
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
    if (((({ const char *tb_s157 = ((tb_o_Token *)tb_nn(v_t, 143))->q_kind; ((unsigned char)tb_s157[0] == 110 && (unsigned char)tb_s157[1] == 117 && (unsigned char)tb_s157[2] == 109 && (unsigned char)tb_s157[3] == 98 && (unsigned char)tb_s157[4] == 101 && (unsigned char)tb_s157[5] == 114 && tb_s157[6] == 0); }) || ({ const char *tb_s158 = ((tb_o_Token *)tb_nn(v_t, 143))->q_kind; ((unsigned char)tb_s158[0] == 100 && (unsigned char)tb_s158[1] == 101 && (unsigned char)tb_s158[2] == 99 && (unsigned char)tb_s158[3] == 105 && (unsigned char)tb_s158[4] == 109 && (unsigned char)tb_s158[5] == 97 && (unsigned char)tb_s158[6] == 108 && tb_s158[7] == 0); })) || ({ const char *tb_s159 = ((tb_o_Token *)tb_nn(v_t, 143))->q_kind; ((unsigned char)tb_s159[0] == 115 && (unsigned char)tb_s159[1] == 116 && (unsigned char)tb_s159[2] == 114 && (unsigned char)tb_s159[3] == 105 && (unsigned char)tb_s159[4] == 110 && (unsigned char)tb_s159[5] == 103 && tb_s159[6] == 0); }))) {
        v_n = f_parser__new_node(((tb_o_Token *)tb_nn(v_t, 144))->q_kind, v_line);
        { const char * tb_v160 = ((tb_o_Token *)tb_nn(v_t, 145))->q_word; const char * *tb_p160 = &(((tb_o_Node *)tb_nn(v_n, 145))->q_value); *tb_p160 = tb_v160; }
        f_parser__next(v_c);
    } else if ((f_parser__is_cmd(v_c, "true") || f_parser__is_cmd(v_c, "false"))) {
        v_n = f_parser__new_node("bool", v_line);
        { const char * tb_v161 = ((tb_o_Token *)tb_nn(v_t, 149))->q_word; const char * *tb_p161 = &(((tb_o_Node *)tb_nn(v_n, 149))->q_value); *tb_p161 = tb_v161; }
        f_parser__next(v_c);
    } else if (({ const char *tb_s162 = ((tb_o_Token *)tb_nn(v_t, 151))->q_kind; ((unsigned char)tb_s162[0] == 110 && (unsigned char)tb_s162[1] == 97 && (unsigned char)tb_s162[2] == 109 && (unsigned char)tb_s162[3] == 101 && tb_s162[4] == 0); })) {
        f_parser__next(v_c);
        if (f_parser__is_sign(v_c, "(")) {
            v_n = f_parser__parse_call(v_c, ((tb_o_Token *)tb_nn(v_t, 154))->q_word, v_line);
        } else {
            v_n = f_parser__new_node("name", v_line);
            { const char * tb_v163 = ((tb_o_Token *)tb_nn(v_t, 157))->q_word; const char * *tb_p163 = &(((tb_o_Node *)tb_nn(v_n, 157))->q_name); *tb_p163 = tb_v163; }
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
                { const char * tb_v164 = v_name; const char * *tb_p164 = &(((tb_o_Node *)tb_nn(v_m, 201))->q_name); *tb_p164 = tb_v164; }
                f_parser__add_kid(v_m, v_n);
                {
                    tb_list * tb_l165 = ((tb_o_Node *)tb_nn(v_call, 203))->q_kids;
                    for (long long tb_i165 = 0; tb_i165 < tb_l165->len; tb_i165++) {
                        tb_o_Node * const v_k = ((tb_o_Node * *)tb_l165->data)[tb_i165];
                        f_parser__add_kid(v_m, v_k);
                    }
                }
                v_n = v_m;
            } else {
                tb_o_Node * v_f = f_parser__new_node("field", v_dot_line);
                { const char * tb_v166 = v_name; const char * *tb_p166 = &(((tb_o_Node *)tb_nn(v_f, 208))->q_name); *tb_p166 = tb_v166; }
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
        { const char * tb_v167 = "-"; const char * *tb_p167 = &(((tb_o_Node *)tb_nn(v_n, 216))->q_value); *tb_p167 = tb_v167; }
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
        { const char * tb_v168 = "not"; const char * *tb_p168 = &(((tb_o_Node *)tb_nn(v_n, 254))->q_value); *tb_p168 = tb_v168; }
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
    if ((({ const char *tb_s169 = ((tb_o_Token *)tb_nn(v_t, 300))->q_kind; ((unsigned char)tb_s169[0] == 110 && (unsigned char)tb_s169[1] == 97 && (unsigned char)tb_s169[2] == 109 && (unsigned char)tb_s169[3] == 101 && tb_s169[4] == 0); }) && ({ const char *tb_s170 = ((tb_o_Token *)tb_nn(v_t, 300))->q_word; ((unsigned char)tb_s170[0] == 111 && (unsigned char)tb_s170[1] == 110 && tb_s170[2] == 0); }))) {
        f_parser__next(v_c);
        tb_o_Token * v_a = f_parser__now(v_c);
        tb_o_Token * v_b = f_parser__ahead(v_c);
        if ((({ const char *tb_s171 = ((tb_o_Token *)tb_nn(v_a, 304))->q_word; !((unsigned char)tb_s171[0] == 97 && (unsigned char)tb_s171[1] == 108 && (unsigned char)tb_s171[2] == 108 && tb_s171[3] == 0); }) || ({ const char *tb_s172 = ((tb_o_Token *)tb_nn(v_b, 304))->q_word; !((unsigned char)tb_s172[0] == 99 && (unsigned char)tb_s172[1] == 111 && (unsigned char)tb_s172[2] == 114 && (unsigned char)tb_s172[3] == 101 && (unsigned char)tb_s172[4] == 115 && tb_s172[5] == 0); }))) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после on пишется all cores: each x in nums on all cores", "");
        }
        f_parser__next(v_c);
        f_parser__next(v_c);
        { _Bool tb_v173 = 1; _Bool *tb_p173 = &(((tb_o_Node *)tb_nn(v_n, 308))->q_is_par); *tb_p173 = tb_v173; }
    }
}

static tb_o_Node * f_parser__parse_each(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_n = f_parser__new_node("each", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "each");
    { const char * tb_v174 = f_parser__expect_name(v_c); const char * *tb_p174 = &(((tb_o_Node *)tb_nn(v_n, 313))->q_name); *tb_p174 = tb_v174; }
    f_parser__expect_cmd(v_c, "in");
    f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
    f_parser__parse_on_all_cores(v_c, v_n);
    if (f_parser__is_cmd(v_c, "by")) {
        f_parser__next(v_c);
        if ((({ const char *tb_s175 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 321))->q_kind; ((unsigned char)tb_s175[0] == 110 && (unsigned char)tb_s175[1] == 97 && (unsigned char)tb_s175[2] == 109 && (unsigned char)tb_s175[3] == 101 && tb_s175[4] == 0); }) && ({ const char *tb_s176 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 321))->q_word; ((unsigned char)tb_s176[0] == 118 && (unsigned char)tb_s176[1] == 101 && (unsigned char)tb_s176[2] == 99 && tb_s176[3] == 0); }))) {
            f_parser__next(v_c);
            { _Bool tb_v177 = 1; _Bool *tb_p177 = &(((tb_o_Node *)tb_nn(v_n, 323))->q_by_vec); *tb_p177 = tb_v177; }
            if (((tb_o_Node *)tb_nn(v_n, 324))->q_is_par) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 325))->q_line, "on all cores и by vec вместе пока нельзя", "");
            }
        } else {
            { tb_o_Node * tb_v178 = f_parser__parse_expr(v_c); tb_o_Node * *tb_p178 = &(((tb_o_Node *)tb_nn(v_n, 328))->q_piece_size); *tb_p178 = tb_v178; }
            { _Bool tb_v179 = 1; _Bool *tb_p179 = &(((tb_o_Node *)tb_nn(v_n, 329))->q_by_piece); *tb_p179 = tb_v179; }
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
    { const char * tb_v180 = f_parser__expect_name(v_c); const char * *tb_p180 = &(((tb_o_Node *)tb_nn(v_n, 339))->q_name); *tb_p180 = tb_v180; }
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
        if ((({ const char *tb_s181 = ((tb_o_Node *)tb_nn(v_call, 371))->q_kind; !((unsigned char)tb_s181[0] == 99 && (unsigned char)tb_s181[1] == 97 && (unsigned char)tb_s181[2] == 108 && (unsigned char)tb_s181[3] == 108 && tb_s181[4] == 0); }) && ({ const char *tb_s182 = ((tb_o_Node *)tb_nn(v_call, 371))->q_kind; !((unsigned char)tb_s182[0] == 109 && (unsigned char)tb_s182[1] == 101 && (unsigned char)tb_s182[2] == 116 && (unsigned char)tb_s182[3] == 104 && (unsigned char)tb_s182[4] == 111 && (unsigned char)tb_s182[5] == 100 && tb_s182[6] == 0); }))) {
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
        { const char * tb_v183 = f_parser__expect_name(v_c); const char * *tb_p183 = &(((tb_o_Node *)tb_nn(v_e, 384))->q_name); *tb_p183 = tb_v183; }
        f_parser__expect_newline(v_c);
        f_parser__add_kid(v_e, v_v);
        f_parser__add_kid(v_e, f_parser__parse_block(v_c));
        return v_e;
    }
    return v_v;
    return v_result;
}

static void f_parser__end_statement(tb_o_Comp * v_c, tb_o_Node * v_value) {
    if (({ const char *tb_s184 = ((tb_o_Node *)tb_nn(v_value, 393))->q_kind; !((unsigned char)tb_s184[0] == 101 && (unsigned char)tb_s184[1] == 108 && (unsigned char)tb_s184[2] == 115 && (unsigned char)tb_s184[3] == 101 && (unsigned char)tb_s184[4] == 95 && (unsigned char)tb_s184[5] == 101 && (unsigned char)tb_s184[6] == 114 && (unsigned char)tb_s184[7] == 114 && tb_s184[8] == 0); })) {
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
    { const char * tb_v185 = f_parser__expect_name(v_c); const char * *tb_p185 = &(((tb_o_Node *)tb_nn(v_n, 403))->q_name); *tb_p185 = tb_v185; }
    { _Bool tb_v186 = v_is_const; _Bool *tb_p186 = &(((tb_o_Node *)tb_nn(v_n, 404))->q_is_const); *tb_p186 = tb_v186; }
    f_parser__expect_sign(v_c, "=");
    if ((f_parser__at_type_word(v_c) || f_parser__is_cmd(v_c, "dict"))) {
        { tb_o_Node * tb_v187 = f_parser__parse_type(v_c); tb_o_Node * *tb_p187 = &(((tb_o_Node *)tb_nn(v_n, 407))->q_tsyn); *tb_p187 = tb_v187; }
        { _Bool tb_v188 = 1; _Bool *tb_p188 = &(((tb_o_Node *)tb_nn(v_n, 408))->q_has_tsyn); *tb_p188 = tb_v188; }
    }
    if (({ const char *tb_s189 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 409))->q_kind; !((unsigned char)tb_s189[0] == 110 && (unsigned char)tb_s189[1] == 101 && (unsigned char)tb_s189[2] == 119 && (unsigned char)tb_s189[3] == 108 && (unsigned char)tb_s189[4] == 105 && (unsigned char)tb_s189[5] == 110 && (unsigned char)tb_s189[6] == 101 && tb_s189[7] == 0); })) {
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
    { const char * tb_v190 = f_parser__expect_name(v_c); const char * *tb_p190 = &(((tb_o_Node *)tb_nn(v_n, 424))->q_name); *tb_p190 = tb_v190; }
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
        { const char * tb_v191 = f_parser__expect_name(v_c); const char * *tb_p191 = &(((tb_o_Node *)tb_nn(v_name, 438))->q_name); *tb_p191 = tb_v191; }
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
        if (((({ const char *tb_s192 = v_k; !((unsigned char)tb_s192[0] == 110 && (unsigned char)tb_s192[1] == 97 && (unsigned char)tb_s192[2] == 109 && (unsigned char)tb_s192[3] == 101 && tb_s192[4] == 0); }) && ({ const char *tb_s193 = v_k; !((unsigned char)tb_s193[0] == 105 && (unsigned char)tb_s193[1] == 110 && (unsigned char)tb_s193[2] == 100 && (unsigned char)tb_s193[3] == 101 && (unsigned char)tb_s193[4] == 120 && tb_s193[5] == 0); })) && ({ const char *tb_s194 = v_k; !((unsigned char)tb_s194[0] == 102 && (unsigned char)tb_s194[1] == 105 && (unsigned char)tb_s194[2] == 101 && (unsigned char)tb_s194[3] == 108 && (unsigned char)tb_s194[4] == 100 && tb_s194[5] == 0); }))) {
            f_errors__error_at(v_c, v_line, "слева от знака должно быть имя, nums[i] или u.age", "");
        }
        tb_o_Node * v_n = f_parser__new_node("set", v_line);
        { const char * tb_v195 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 464))->q_word; const char * *tb_p195 = &(((tb_o_Node *)tb_nn(v_n, 464))->q_value); *tb_p195 = tb_v195; }
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
        { const char * tb_v196 = f_parser__expect_name(v_c); const char * *tb_p196 = &(((tb_o_Node *)tb_nn(v_e, 475))->q_name); *tb_p196 = tb_v196; }
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
        tb_list * tb_l197 = tb_list_from(4, sizeof(const char *), (const char *[]){"gives", "obj", "danger", "by"});
        for (long long tb_i197 = 0; tb_i197 < tb_l197->len; tb_i197++) {
            const char * const v_w = ((const char * *)tb_l197->data)[tb_i197];
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
        if (({ const char *tb_s198 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 509))->q_kind; !((unsigned char)tb_s198[0] == 110 && (unsigned char)tb_s198[1] == 101 && (unsigned char)tb_s198[2] == 119 && (unsigned char)tb_s198[3] == 108 && (unsigned char)tb_s198[4] == 105 && (unsigned char)tb_s198[5] == 110 && (unsigned char)tb_s198[6] == 101 && tb_s198[7] == 0); })) {
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
        if ((({ const char *tb_s199 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 547))->q_kind; !((unsigned char)tb_s199[0] == 110 && (unsigned char)tb_s199[1] == 97 && (unsigned char)tb_s199[2] == 109 && (unsigned char)tb_s199[3] == 101 && tb_s199[4] == 0); }) || (!f_parser__ahead_is_sign(v_c, "=")))) {
            f_errors__error_at(v_c, v_line, "после shared пишется новая переменная: shared y = int 0", "");
        }
        tb_o_Node * v_n = f_parser__parse_assign(v_c, 0);
        { _Bool tb_v200 = 1; _Bool *tb_p200 = &(((tb_o_Node *)tb_nn(v_n, 550))->q_is_shared); *tb_p200 = tb_v200; }
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
    if (({ const char *tb_s201 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 562))->q_kind; ((unsigned char)tb_s201[0] == 110 && (unsigned char)tb_s201[1] == 97 && (unsigned char)tb_s201[2] == 109 && (unsigned char)tb_s201[3] == 101 && tb_s201[4] == 0); })) {
        return f_parser__parse_name_statement(v_c);
    }
    if ((({ const char *tb_s202 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 564))->q_kind; ((unsigned char)tb_s202[0] == 99 && (unsigned char)tb_s202[1] == 111 && (unsigned char)tb_s202[2] == 109 && (unsigned char)tb_s202[3] == 109 && (unsigned char)tb_s202[4] == 97 && (unsigned char)tb_s202[5] == 110 && (unsigned char)tb_s202[6] == 100 && tb_s202[7] == 0); }) && f_parser__ahead_is_sign(v_c, "="))) {
        f_errors__error_at(v_c, v_line, "это слово занято языком, возьми другое имя", ((tb_o_Token *)tb_nn(f_parser__now(v_c), 565))->q_word);
    }
    f_errors__error_at(v_c, v_line, "не понимаю, что делает строка, начиная с", f_parser__what_is_here(v_c));
    return f_parser__new_node("missing", v_line);
    return v_result;
}

static tb_o_Node * f_parser__parse_block(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_block = f_parser__new_node("block", f_parser__line_now(v_c));
    if (({ const char *tb_s203 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 571))->q_kind; !((unsigned char)tb_s203[0] == 105 && (unsigned char)tb_s203[1] == 110 && (unsigned char)tb_s203[2] == 100 && (unsigned char)tb_s203[3] == 101 && (unsigned char)tb_s203[4] == 110 && (unsigned char)tb_s203[5] == 116 && tb_s203[6] == 0); })) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "после этой строки нужен блок с отступом", "");
    }
    f_parser__next(v_c);
    while ((({ const char *tb_s204 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 574))->q_kind; !((unsigned char)tb_s204[0] == 100 && (unsigned char)tb_s204[1] == 101 && (unsigned char)tb_s204[2] == 100 && (unsigned char)tb_s204[3] == 101 && (unsigned char)tb_s204[4] == 110 && (unsigned char)tb_s204[5] == 116 && tb_s204[6] == 0); }) && ({ const char *tb_s205 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 574))->q_kind; !((unsigned char)tb_s205[0] == 101 && (unsigned char)tb_s205[1] == 110 && (unsigned char)tb_s205[2] == 100 && tb_s205[3] == 0); }))) {
        if (({ const char *tb_s206 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 575))->q_kind; ((unsigned char)tb_s206[0] == 105 && (unsigned char)tb_s206[1] == 110 && (unsigned char)tb_s206[2] == 100 && (unsigned char)tb_s206[3] == 101 && (unsigned char)tb_s206[4] == 110 && (unsigned char)tb_s206[5] == 116 && tb_s206[6] == 0); })) {
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
        { _Bool tb_v207 = 1; _Bool *tb_p207 = &(((tb_o_Node *)tb_nn(v_p, 588))->q_is_link); *tb_p207 = tb_v207; }
        { const char * tb_v208 = f_parser__expect_name(v_c); const char * *tb_p208 = &(((tb_o_Node *)tb_nn(v_p, 589))->q_name); *tb_p208 = tb_v208; }
        return v_p;
    }
    { const char * tb_v209 = f_parser__expect_name(v_c); const char * *tb_p209 = &(((tb_o_Node *)tb_nn(v_p, 591))->q_name); *tb_p209 = tb_v209; }
    f_parser__expect_sign(v_c, ":");
    { tb_o_Node * tb_v210 = f_parser__parse_type(v_c); tb_o_Node * *tb_p210 = &(((tb_o_Node *)tb_nn(v_p, 593))->q_tsyn); *tb_p210 = tb_v210; }
    { _Bool tb_v211 = 1; _Bool *tb_p211 = &(((tb_o_Node *)tb_nn(v_p, 594))->q_has_tsyn); *tb_p211 = tb_v211; }
    return v_p;
    return v_result;
}

static tb_o_Node * f_parser__parse_func(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_f = f_parser__new_node("func", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "func");
    { const char * tb_v212 = f_parser__expect_name(v_c); const char * *tb_p212 = &(((tb_o_Node *)tb_nn(v_f, 601))->q_name); *tb_p212 = tb_v212; }
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
    { const char * tb_v213 = f_parser__expect_name(v_c); const char * *tb_p213 = &(((tb_o_Node *)tb_nn(v_o, 630))->q_name); *tb_p213 = tb_v213; }
    if (f_parser__is_cmd(v_c, "by")) {
        f_parser__next(v_c);
        if ((({ const char *tb_s214 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 634))->q_kind; !((unsigned char)tb_s214[0] == 110 && (unsigned char)tb_s214[1] == 97 && (unsigned char)tb_s214[2] == 109 && (unsigned char)tb_s214[3] == 101 && tb_s214[4] == 0); }) || ({ const char *tb_s215 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 634))->q_word; !((unsigned char)tb_s215[0] == 99 && (unsigned char)tb_s215[1] == 111 && (unsigned char)tb_s215[2] == 108 && (unsigned char)tb_s215[3] == 117 && (unsigned char)tb_s215[4] == 109 && (unsigned char)tb_s215[5] == 110 && (unsigned char)tb_s215[6] == 115 && tb_s215[7] == 0); }))) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после obj ... by пишется columns: obj Point by columns", "");
        }
        f_parser__next(v_c);
        { _Bool tb_v216 = 1; _Bool *tb_p216 = &(((tb_o_Node *)tb_nn(v_o, 637))->q_by_cols); *tb_p216 = tb_v216; }
    }
    f_parser__expect_newline(v_c);
    if (({ const char *tb_s217 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 639))->q_kind; !((unsigned char)tb_s217[0] == 105 && (unsigned char)tb_s217[1] == 110 && (unsigned char)tb_s217[2] == 100 && (unsigned char)tb_s217[3] == 101 && (unsigned char)tb_s217[4] == 110 && (unsigned char)tb_s217[5] == 116 && tb_s217[6] == 0); })) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "у obj ниже с отступом пишутся качества: name: text", "");
    }
    f_parser__next(v_c);
    while ((({ const char *tb_s218 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 642))->q_kind; !((unsigned char)tb_s218[0] == 100 && (unsigned char)tb_s218[1] == 101 && (unsigned char)tb_s218[2] == 100 && (unsigned char)tb_s218[3] == 101 && (unsigned char)tb_s218[4] == 110 && (unsigned char)tb_s218[5] == 116 && tb_s218[6] == 0); }) && ({ const char *tb_s219 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 642))->q_kind; !((unsigned char)tb_s219[0] == 101 && (unsigned char)tb_s219[1] == 110 && (unsigned char)tb_s219[2] == 100 && tb_s219[3] == 0); }))) {
        f_parser__add_kid(v_o, f_parser__parse_param(v_c, 0));
        f_parser__expect_newline(v_c);
    }
    f_parser__next(v_c);
    return v_o;
    return v_result;
}

static tb_o_Node * f_parser__parse(tb_o_Comp * v_c) {
    { long long tb_v220 = 0LL; long long *tb_p220 = &(((tb_o_Comp *)tb_nn(v_c, 649))->q_pos); *tb_p220 = tb_v220; }
    tb_o_Node * v_program = f_parser__new_node("program", 1LL);
    while (({ const char *tb_s221 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 651))->q_kind; !((unsigned char)tb_s221[0] == 101 && (unsigned char)tb_s221[1] == 110 && (unsigned char)tb_s221[2] == 100 && tb_s221[3] == 0); })) {
        if (f_parser__is_cmd(v_c, "use")) {
            tb_o_Node * v_u = f_parser__new_node("use", f_parser__line_now(v_c));
            f_parser__next(v_c);
            { const char * tb_v222 = f_parser__expect_name(v_c); const char * *tb_p222 = &(((tb_o_Node *)tb_nn(v_u, 656))->q_name); *tb_p222 = tb_v222; }
            f_parser__expect_newline(v_c);
            f_parser__add_kid(v_program, v_u);
        } else if ((f_parser__is_cmd(v_c, "modpub") || f_parser__is_cmd(v_c, "modpri"))) {
            _Bool v_is_pub = f_parser__is_cmd(v_c, "modpub");
            f_parser__next(v_c);
            if ((!f_parser__is_cmd(v_c, "func"))) {
                f_errors__error_at(v_c, f_parser__line_now(v_c), "modpub и modpri ставятся перед func", "");
            }
            tb_o_Node * v_f = f_parser__parse_func(v_c);
            { _Bool tb_v223 = v_is_pub; _Bool *tb_p223 = &(((tb_o_Node *)tb_nn(v_f, 665))->q_is_pub); *tb_p223 = tb_v223; }
            f_parser__add_kid(v_program, v_f);
        } else if (f_parser__is_cmd(v_c, "func")) {
            f_parser__add_kid(v_program, f_parser__parse_func(v_c));
        } else if (f_parser__is_cmd(v_c, "obj")) {
            f_parser__add_kid(v_program, f_parser__parse_obj(v_c));
        } else if (f_parser__is_cmd(v_c, "link")) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "подключение C-файлов через link будет в версии 0.9 (вместе с danger); файлы Tachbit подключает use", "");
        } else if (({ const char *tb_s224 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 673))->q_kind; ((unsigned char)tb_s224[0] == 105 && (unsigned char)tb_s224[1] == 110 && (unsigned char)tb_s224[2] == 100 && (unsigned char)tb_s224[3] == 101 && (unsigned char)tb_s224[4] == 110 && (unsigned char)tb_s224[5] == 116 && tb_s224[6] == 0); })) {
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
        long long tb_end225 = v_depth;
        for (long long v_i = 1LL; v_i <= tb_end225; v_i++) {
            tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, "  ", sizeof("  ") - 1); 
        }
    }
    tb_app(&v_s, &tb_rl_s, &tb_rc_s, f_parser__node_kind_name(((tb_o_Node *)tb_nn(v_n, 689))->q_kind)); 
    if (({ const char *tb_s226 = ((tb_o_Node *)tb_nn(v_n, 690))->q_name; !(tb_s226[0] == 0); })) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, " ", sizeof(" ") - 1); tb_app(&v_s, &tb_rl_s, &tb_rc_s, ((tb_o_Node *)tb_nn(v_n, 691))->q_name); 
    }
    if (({ const char *tb_s227 = ((tb_o_Node *)tb_nn(v_n, 692))->q_value; !(tb_s227[0] == 0); })) {
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
    { const char * tb_v228 = (tb_rl_s = -1, v_s); *(const char * *)tb_grow(v_out) = tb_v228; }
    if (((tb_o_Node *)tb_nn(v_n, 707))->q_has_tsyn) {
        f_parser__tree_lines(((tb_o_Node *)tb_nn(v_n, 708))->q_tsyn, (v_depth + 1LL), v_out);
    }
    {
        tb_list * tb_l229 = ((tb_o_Node *)tb_nn(v_n, 709))->q_kids;
        for (long long tb_i229 = 0; tb_i229 < tb_l229->len; tb_i229++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l229->data)[tb_i229];
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
        v_func_key = ({ const char *tb_c230_0 = ((tb_o_Node *)tb_nn(v_fnode, 17))->q_mod; long long tb_c230_0n = (long long)strlen(tb_c230_0); const char *tb_c230_1 = "."; long long tb_c230_1n = sizeof(".") - 1; const char *tb_c230_2 = ((tb_o_Node *)tb_nn(v_fnode, 17))->q_name; long long tb_c230_2n = (long long)strlen(tb_c230_2); char *tb_c230r = tb_mem(tb_c230_0n + tb_c230_1n + tb_c230_2n + 1), *tb_c230rw = tb_c230r; memcpy(tb_c230rw, tb_c230_0, tb_c230_0n); tb_c230rw += tb_c230_0n; memcpy(tb_c230rw, tb_c230_1, tb_c230_1n); tb_c230rw += tb_c230_1n; memcpy(tb_c230rw, tb_c230_2, tb_c230_2n); tb_c230rw += tb_c230_2n; *tb_c230rw = 0; (const char *)tb_c230r; });
    }
    {
        tb_list * tb_l231 = ((tb_o_Comp *)tb_nn(v_c, 18))->q_types;
        for (long long tb_i231 = 0; tb_i231 < tb_l231->len; tb_i231++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l231->data)[tb_i231];
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
        { tb_o_Node * tb_v232 = v_onode; tb_o_Node * *tb_p232 = &(((tb_o_Type *)tb_nn(v_t, 25))->q_obj_node); *tb_p232 = tb_v232; }
    }
    if (v_has_func) {
        { tb_o_Node * tb_v233 = v_fnode; tb_o_Node * *tb_p233 = &(((tb_o_Type *)tb_nn(v_t, 27))->q_func_node); *tb_p233 = tb_v233; }
    }
    { tb_o_Type * tb_v234 = v_t; *(tb_o_Type * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 28))->q_types) = tb_v234; }
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
    if (({ const char *tb_s235 = ((tb_o_Type *)tb_nn(v_ty, 65))->q_kind; ((unsigned char)tb_s235[0] == 108 && (unsigned char)tb_s235[1] == 105 && (unsigned char)tb_s235[2] == 115 && (unsigned char)tb_s235[3] == 116 && tb_s235[4] == 0); })) {
        return ({ const char *tb_c236_0 = "list "; long long tb_c236_0n = sizeof("list ") - 1; const char *tb_c236_1 = f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 66))->q_elem); long long tb_c236_1n = (long long)strlen(tb_c236_1); char *tb_c236r = tb_mem(tb_c236_0n + tb_c236_1n + 1), *tb_c236rw = tb_c236r; memcpy(tb_c236rw, tb_c236_0, tb_c236_0n); tb_c236rw += tb_c236_0n; memcpy(tb_c236rw, tb_c236_1, tb_c236_1n); tb_c236rw += tb_c236_1n; *tb_c236rw = 0; (const char *)tb_c236r; });
    }
    if (({ const char *tb_s237 = ((tb_o_Type *)tb_nn(v_ty, 67))->q_kind; ((unsigned char)tb_s237[0] == 100 && (unsigned char)tb_s237[1] == 105 && (unsigned char)tb_s237[2] == 99 && (unsigned char)tb_s237[3] == 116 && tb_s237[4] == 0); })) {
        return ({ const char *tb_c238_0 = "dict "; long long tb_c238_0n = sizeof("dict ") - 1; const char *tb_c238_1 = f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 68))->q_key); long long tb_c238_1n = (long long)strlen(tb_c238_1); const char *tb_c238_2 = " "; long long tb_c238_2n = sizeof(" ") - 1; const char *tb_c238_3 = f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 68))->q_elem); long long tb_c238_3n = (long long)strlen(tb_c238_3); char *tb_c238r = tb_mem(tb_c238_0n + tb_c238_1n + tb_c238_2n + tb_c238_3n + 1), *tb_c238rw = tb_c238r; memcpy(tb_c238rw, tb_c238_0, tb_c238_0n); tb_c238rw += tb_c238_0n; memcpy(tb_c238rw, tb_c238_1, tb_c238_1n); tb_c238rw += tb_c238_1n; memcpy(tb_c238rw, tb_c238_2, tb_c238_2n); tb_c238rw += tb_c238_2n; memcpy(tb_c238rw, tb_c238_3, tb_c238_3n); tb_c238rw += tb_c238_3n; *tb_c238rw = 0; (const char *)tb_c238r; });
    }
    if (({ const char *tb_s239 = ((tb_o_Type *)tb_nn(v_ty, 69))->q_kind; ((unsigned char)tb_s239[0] == 111 && (unsigned char)tb_s239[1] == 98 && (unsigned char)tb_s239[2] == 106 && tb_s239[3] == 0); })) {
        return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn(v_ty, 70))->q_obj_node, 70))->q_name;
    }
    if (({ const char *tb_s240 = ((tb_o_Type *)tb_nn(v_ty, 71))->q_kind; ((unsigned char)tb_s240[0] == 109 && (unsigned char)tb_s240[1] == 117 && (unsigned char)tb_s240[2] == 108 && (unsigned char)tb_s240[3] == 116 && (unsigned char)tb_s240[4] == 105 && tb_s240[5] == 0); })) {
        return "несколько выходов";
    }
    if (({ const char *tb_s241 = ((tb_o_Type *)tb_nn(v_ty, 73))->q_kind; ((unsigned char)tb_s241[0] == 110 && (unsigned char)tb_s241[1] == 111 && (unsigned char)tb_s241[2] == 110 && (unsigned char)tb_s241[3] == 101 && tb_s241[4] == 0); })) {
        return "ничего";
    }
    if (({ const char *tb_s242 = ((tb_o_Type *)tb_nn(v_ty, 75))->q_kind; ((unsigned char)tb_s242[0] == 116 && (unsigned char)tb_s242[1] == 97 && (unsigned char)tb_s242[2] == 115 && (unsigned char)tb_s242[3] == 107 && tb_s242[4] == 0); })) {
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
    return ({ const char *tb_s243 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s243[0] == 108 && (unsigned char)tb_s243[1] == 105 && (unsigned char)tb_s243[2] == 115 && (unsigned char)tb_s243[3] == 116 && tb_s243[4] == 0); });
    tb_crash(82, "функция is_list дошла до конца без return");
    return 0;
}

static _Bool f_check__is_dict(tb_o_Comp * v_c, long long v_t) {
    return ({ const char *tb_s244 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s244[0] == 100 && (unsigned char)tb_s244[1] == 105 && (unsigned char)tb_s244[2] == 99 && (unsigned char)tb_s244[3] == 116 && tb_s244[4] == 0); });
    tb_crash(85, "функция is_dict дошла до конца без return");
    return 0;
}

static _Bool f_check__is_cols_obj(tb_o_Comp * v_c, long long v_t) {
    return (({ const char *tb_s245 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s245[0] == 111 && (unsigned char)tb_s245[1] == 98 && (unsigned char)tb_s245[2] == 106 && tb_s245[3] == 0); }) && ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 90))->q_types, v_t, sizeof(tb_o_Type *), 90)), 90))->q_obj_node, 90))->q_by_cols);
    tb_crash(89, "функция is_cols_obj дошла до конца без return");
    return 0;
}

static _Bool f_check__is_cols_list(tb_o_Comp * v_c, long long v_t) {
    return (({ const char *tb_s246 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s246[0] == 108 && (unsigned char)tb_s246[1] == 105 && (unsigned char)tb_s246[2] == 115 && (unsigned char)tb_s246[3] == 116 && tb_s246[4] == 0); }) && f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 94))->q_types, v_t, sizeof(tb_o_Type *), 94)), 94))->q_elem));
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
    {
        tb_list * tb_l247 = tb_list_from(29, sizeof(const char *), (const char *[]){"print", "len", "round", "code", "char", "cut", "to_int", "to_dec", "read_file", "write_file", "input", "args", "split", "join", "find", "replace", "upper", "lower", "trim", "starts_with", "ends_with", "sort", "abs", "min", "max", "sqrt", "run", "exit", "file_exists"});
        for (long long tb_i247 = 0; tb_i247 < tb_l247->len; tb_i247++) {
            const char * const v_b = ((const char * *)tb_l247->data)[tb_i247];
            if ((strcmp(v_b, v_name) == 0)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(109, "функция is_builtin дошла до конца без return");
    return 0;
}

static _Bool f_check__builtin_can_fail(const char * v_name) {
    return ((((({ const char *tb_s248 = v_name; ((unsigned char)tb_s248[0] == 116 && (unsigned char)tb_s248[1] == 111 && (unsigned char)tb_s248[2] == 95 && (unsigned char)tb_s248[3] == 105 && (unsigned char)tb_s248[4] == 110 && (unsigned char)tb_s248[5] == 116 && tb_s248[6] == 0); }) || ({ const char *tb_s249 = v_name; ((unsigned char)tb_s249[0] == 116 && (unsigned char)tb_s249[1] == 111 && (unsigned char)tb_s249[2] == 95 && (unsigned char)tb_s249[3] == 100 && (unsigned char)tb_s249[4] == 101 && (unsigned char)tb_s249[5] == 99 && tb_s249[6] == 0); })) || (strcmp(v_name, "read_file") == 0)) || (strcmp(v_name, "write_file") == 0)) || ({ const char *tb_s250 = v_name; ((unsigned char)tb_s250[0] == 105 && (unsigned char)tb_s250[1] == 110 && (unsigned char)tb_s250[2] == 112 && (unsigned char)tb_s250[3] == 117 && (unsigned char)tb_s250[4] == 116 && tb_s250[5] == 0); }));
    tb_crash(116, "функция builtin_can_fail дошла до конца без return");
    return 0;
}

static _Bool f_check__is_module(tb_o_Comp * v_c, const char * v_name) {
    {
        tb_list * tb_l251 = ((tb_o_Comp *)tb_nn(v_c, 123))->q_all_mods;
        for (long long tb_i251 = 0; tb_i251 < tb_l251->len; tb_i251++) {
            const char * const v_m = ((const char * *)tb_l251->data)[tb_i251];
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
        tb_list * tb_l252 = ((tb_o_Comp *)tb_nn(v_c, 131))->q_uses;
        for (long long tb_i252 = 0; tb_i252 < tb_l252->len; tb_i252++) {
            tb_o_Use * const v_u = ((tb_o_Use * *)tb_l252->data)[tb_i252];
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
        long long tb_end253 = ((((tb_o_Comp *)tb_nn(v_c, 138))->q_funcs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end253; v_i++) {
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
        long long tb_end254 = ((((tb_o_Comp *)tb_nn(v_c, 150))->q_funcs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end254; v_i++) {
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
        tb_list * tb_l255 = ((tb_o_Comp *)tb_nn(v_c, 160))->q_funcs;
        for (long long tb_i255 = 0; tb_i255 < tb_l255->len; tb_i255++) {
            tb_o_Func * const v_f = ((tb_o_Func * *)tb_l255->data)[tb_i255];
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
        long long tb_end256 = ((((tb_o_Comp *)tb_nn(v_c, 169))->q_objs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end256; v_i++) {
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
        long long tb_end257 = ((((tb_o_Node *)tb_nn(v_o, 175))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end257; v_i++) {
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
    if (({ const char *tb_s258 = ((tb_o_Node *)tb_nn(v_t, 181))->q_name; ((unsigned char)tb_s258[0] == 105 && (unsigned char)tb_s258[1] == 110 && (unsigned char)tb_s258[2] == 116 && tb_s258[3] == 0); })) {
        return 1LL;
    }
    if (({ const char *tb_s259 = ((tb_o_Node *)tb_nn(v_t, 183))->q_name; ((unsigned char)tb_s259[0] == 100 && (unsigned char)tb_s259[1] == 101 && (unsigned char)tb_s259[2] == 99 && tb_s259[3] == 0); })) {
        return 2LL;
    }
    if (({ const char *tb_s260 = ((tb_o_Node *)tb_nn(v_t, 185))->q_name; ((unsigned char)tb_s260[0] == 116 && (unsigned char)tb_s260[1] == 101 && (unsigned char)tb_s260[2] == 120 && (unsigned char)tb_s260[3] == 116 && tb_s260[4] == 0); })) {
        return 4LL;
    }
    if (({ const char *tb_s261 = ((tb_o_Node *)tb_nn(v_t, 187))->q_name; ((unsigned char)tb_s261[0] == 98 && (unsigned char)tb_s261[1] == 111 && (unsigned char)tb_s261[2] == 111 && (unsigned char)tb_s261[3] == 108 && tb_s261[4] == 0); })) {
        return 3LL;
    }
    if (({ const char *tb_s262 = ((tb_o_Node *)tb_nn(v_t, 189))->q_name; ((unsigned char)tb_s262[0] == 108 && (unsigned char)tb_s262[1] == 105 && (unsigned char)tb_s262[2] == 115 && (unsigned char)tb_s262[3] == 116 && tb_s262[4] == 0); })) {
        return f_check__list_of(v_c, f_check__resolve_type(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 190))->q_kids, 0LL, sizeof(tb_o_Node *), 190))));
    }
    if (({ const char *tb_s263 = ((tb_o_Node *)tb_nn(v_t, 191))->q_name; ((unsigned char)tb_s263[0] == 100 && (unsigned char)tb_s263[1] == 105 && (unsigned char)tb_s263[2] == 99 && (unsigned char)tb_s263[3] == 116 && tb_s263[4] == 0); })) {
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
        { long long tb_v264 = 1LL; long long *tb_p264 = &(v_i); *tb_p264 -= tb_v264; }
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
    { tb_o_Var * tb_v265 = tb_new_Var(v_name, v_type, v_is_const, v_is_link, ((tb_o_Comp *)tb_nn(v_c, 224))->q_depth, 0, ""); *(tb_o_Var * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 224))->q_vars) = tb_v265; }
}

static void f_check__cut_vars(tb_o_Comp * v_c, long long v_count_left) {
    while (((((tb_o_Comp *)tb_nn(v_c, 227))->q_vars)->len > v_count_left)) {
        tb_o_Var * v_gone = (*(tb_o_Var * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 228))->q_vars, 228));
    }
}

static void f_check__open_scope(tb_o_Comp * v_c) {
    { long long tb_v266 = 1LL; long long *tb_p266 = &(((tb_o_Comp *)tb_nn(v_c, 231))->q_depth); *tb_p266 += tb_v266; }
}

static void f_check__close_scope(tb_o_Comp * v_c) {
    while ((((((tb_o_Comp *)tb_nn(v_c, 234))->q_vars)->len > 0LL) && (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 234))->q_vars, ((((tb_o_Comp *)tb_nn(v_c, 234))->q_vars)->len - 1LL), sizeof(tb_o_Var *), 234)), 234))->q_depth == ((tb_o_Comp *)tb_nn(v_c, 234))->q_depth))) {
        tb_o_Var * v_gone = (*(tb_o_Var * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 235))->q_vars, 235));
    }
    { long long tb_v267 = 1LL; long long *tb_p267 = &(((tb_o_Comp *)tb_nn(v_c, 236))->q_depth); *tb_p267 -= tb_v267; }
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
    if (((({ const char *tb_s268 = ((tb_o_Node *)tb_nn(v_n, 251))->q_kind; ((unsigned char)tb_s268[0] == 105 && (unsigned char)tb_s268[1] == 110 && (unsigned char)tb_s268[2] == 100 && (unsigned char)tb_s268[3] == 101 && (unsigned char)tb_s268[4] == 120 && tb_s268[5] == 0); }) || ({ const char *tb_s269 = ((tb_o_Node *)tb_nn(v_n, 251))->q_kind; ((unsigned char)tb_s269[0] == 102 && (unsigned char)tb_s269[1] == 105 && (unsigned char)tb_s269[2] == 101 && (unsigned char)tb_s269[3] == 108 && (unsigned char)tb_s269[4] == 100 && tb_s269[5] == 0); })) || ({ const char *tb_s270 = ((tb_o_Node *)tb_nn(v_n, 251))->q_kind; ((unsigned char)tb_s270[0] == 109 && (unsigned char)tb_s270[1] == 101 && (unsigned char)tb_s270[2] == 116 && (unsigned char)tb_s270[3] == 104 && (unsigned char)tb_s270[4] == 111 && (unsigned char)tb_s270[5] == 100 && tb_s270[6] == 0); }))) {
        return f_check__root_var(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 252))->q_kids, 0LL, sizeof(tb_o_Node *), 252)));
    }
    if (({ const char *tb_s271 = ((tb_o_Node *)tb_nn(v_n, 253))->q_kind; ((unsigned char)tb_s271[0] == 110 && (unsigned char)tb_s271[1] == 97 && (unsigned char)tb_s271[2] == 109 && (unsigned char)tb_s271[3] == 101 && tb_s271[4] == 0); })) {
        return f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 254))->q_name);
    }
    return (-1LL);
    return v_r;
}

static void f_check__capture(tb_o_Comp * v_c, long long v_v) {
    tb_o_Var * v_var = (*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 259))->q_vars, v_v, sizeof(tb_o_Var *), 259));
    {
        tb_list * tb_l272 = ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 260))->q_par_node, 260))->q_caps;
        for (long long tb_i272 = 0; tb_i272 < tb_l272->len; tb_i272++) {
            tb_o_Var * const v_old = ((tb_o_Var * *)tb_l272->data)[tb_i272];
            if (((strcmp(((tb_o_Var *)tb_nn(v_old, 261))->q_name, ((tb_o_Var *)tb_nn(v_var, 261))->q_name) == 0) && ((!((tb_o_Comp *)tb_nn(v_c, 261))->q_par_vec) || (!((tb_o_Var *)tb_nn(v_old, 261))->q_is_shared)))) {
                return;
            }
        }
    }
    { tb_o_Var * tb_v273 = tb_new_Var(((tb_o_Var *)tb_nn(v_var, 263))->q_name, ((tb_o_Var *)tb_nn(v_var, 263))->q_type, 0, ((tb_o_Var *)tb_nn(v_var, 263))->q_is_link, 0LL, (((tb_o_Var *)tb_nn(v_var, 263))->q_is_shared && (!((tb_o_Comp *)tb_nn(v_c, 263))->q_par_vec)), ""); *(tb_o_Var * *)tb_grow(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 263))->q_par_node, 263))->q_caps) = tb_v273; }
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
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 288))->q_line, ({ const char *tb_c274_0 = "внутри "; long long tb_c274_0n = sizeof("внутри ") - 1; const char *tb_c274_1 = f_check__loop_word(v_c); long long tb_c274_1n = (long long)strlen(tb_c274_1); const char *tb_c274_2 = " нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)"; long long tb_c274_2n = sizeof(" нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)") - 1; char *tb_c274r = tb_mem(tb_c274_0n + tb_c274_1n + tb_c274_2n + 1), *tb_c274rw = tb_c274r; memcpy(tb_c274rw, tb_c274_0, tb_c274_0n); tb_c274rw += tb_c274_0n; memcpy(tb_c274rw, tb_c274_1, tb_c274_1n); tb_c274rw += tb_c274_1n; memcpy(tb_c274rw, tb_c274_2, tb_c274_2n); tb_c274rw += tb_c274_2n; *tb_c274rw = 0; (const char *)tb_c274r; }), "");
    }
}

static void f_check__need_value(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t) {
    if ((v_t == 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 294))->q_line, "это ничего не возвращает, значение нельзя использовать", ((tb_o_Node *)tb_nn(v_n, 294))->q_name);
    }
    if (({ const char *tb_s275 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s275[0] == 109 && (unsigned char)tb_s275[1] == 117 && (unsigned char)tb_s275[2] == 108 && (unsigned char)tb_s275[3] == 116 && (unsigned char)tb_s275[4] == 105 && tb_s275[5] == 0); })) {
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
    if ((({ const char *tb_s276 = v_op; ((unsigned char)tb_s276[0] == 111 && (unsigned char)tb_s276[1] == 114 && tb_s276[2] == 0); }) && f_check__can_fail_call(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 305))->q_kids, 0LL, sizeof(tb_o_Node *), 305))))) {
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
        { long long tb_v277 = v_a; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 315))->q_temp_types) = tb_v277; }
        { long long tb_v278 = ((((tb_o_Comp *)tb_nn(v_c, 316))->q_temp_types)->len - 1LL); long long *tb_p278 = &(((tb_o_Node *)tb_nn(v_n, 316))->q_temp_no); *tb_p278 = tb_v278; }
        { const char * tb_v279 = "orelse"; const char * *tb_p279 = &(((tb_o_Node *)tb_nn(v_n, 317))->q_value); *tb_p279 = tb_v279; }
        return v_a;
    }
    long long v_a = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 319))->q_kids, 0LL, sizeof(tb_o_Node *), 319)));
    long long v_b = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 320))->q_kids, 1LL, sizeof(tb_o_Node *), 320)));
    if ((({ const char *tb_s280 = v_op; ((unsigned char)tb_s280[0] == 97 && (unsigned char)tb_s280[1] == 110 && (unsigned char)tb_s280[2] == 100 && tb_s280[3] == 0); }) || ({ const char *tb_s281 = v_op; ((unsigned char)tb_s281[0] == 111 && (unsigned char)tb_s281[1] == 114 && tb_s281[2] == 0); }))) {
        if (((v_a != 3LL) || (v_b != 3LL))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 323))->q_line, "and / or работают только с true и false", "");
        }
        return 3LL;
    }
    if ((({ const char *tb_s282 = v_op; ((unsigned char)tb_s282[0] == 61 && (unsigned char)tb_s282[1] == 61 && tb_s282[2] == 0); }) || ({ const char *tb_s283 = v_op; ((unsigned char)tb_s283[0] == 33 && (unsigned char)tb_s283[1] == 61 && tb_s283[2] == 0); }))) {
        if (((!(f_check__is_number(v_a) && f_check__is_number(v_b))) && (v_a != v_b))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 327))->q_line, "нельзя сравнить разные типы", "");
        }
        if ((!f_check__is_scalar(v_a))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 329))->q_line, "списки и объекты пока нельзя сравнивать", "");
        }
        return 3LL;
    }
    if ((((({ const char *tb_s284 = v_op; ((unsigned char)tb_s284[0] == 60 && tb_s284[1] == 0); }) || ({ const char *tb_s285 = v_op; ((unsigned char)tb_s285[0] == 62 && tb_s285[1] == 0); })) || ({ const char *tb_s286 = v_op; ((unsigned char)tb_s286[0] == 60 && (unsigned char)tb_s286[1] == 61 && tb_s286[2] == 0); })) || ({ const char *tb_s287 = v_op; ((unsigned char)tb_s287[0] == 62 && (unsigned char)tb_s287[1] == 61 && tb_s287[2] == 0); }))) {
        if (((v_a == 4LL) && (v_b == 4LL))) {
            return 3LL;
        }
        if (((!f_check__is_number(v_a)) || (!f_check__is_number(v_b)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 335))->q_line, "больше / меньше работают с числами или с двумя текстами", v_op);
        }
        return 3LL;
    }
    if ((({ const char *tb_s288 = v_op; ((unsigned char)tb_s288[0] == 43 && tb_s288[1] == 0); }) && ((v_a == 4LL) || (v_b == 4LL)))) {
        if (((!f_check__is_scalar(v_a)) || (!f_check__is_scalar(v_b)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 339))->q_line, "список или объект нельзя сложить с текстом", "");
        }
        return 4LL;
    }
    if (((!f_check__is_number(v_a)) || (!f_check__is_number(v_b)))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 342))->q_line, "эта операция работает только с числами", v_op);
    }
    if ((({ const char *tb_s289 = v_op; ((unsigned char)tb_s289[0] == 37 && tb_s289[1] == 0); }) && ((v_a == 2LL) || (v_b == 2LL)))) {
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
        long long tb_end290 = ((((tb_o_Node *)tb_nn(v_n, 353))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end290; v_i++) {
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
    if (({ const char *tb_s291 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 365))->q_kids, v_i, sizeof(tb_o_Node *), 365)), 365))->q_name; !(tb_s291[0] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 366))->q_line, "у встроенных функций аргументы пишутся без имён", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 366))->q_kids, v_i, sizeof(tb_o_Node *), 366)), 366))->q_name);
    }
    long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 367))->q_kids, v_i, sizeof(tb_o_Node *), 367)), 367))->q_kids, 0LL, sizeof(tb_o_Node *), 367)));
    { long long tb_v292 = v_t; long long *tb_p292 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 368))->q_kids, v_i, sizeof(tb_o_Node *), 368)), 368))->q_ty); *tb_p292 = tb_v292; }
    return v_t;
    return v_r;
}

static void f_check__builtin_args(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_pattern, const char * v_usage) {
    if (((((tb_o_Node *)tb_nn(v_n, 374))->q_kids)->len != tb_tlen(v_pattern))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 375))->q_line, "не то количество значений, правильно так", v_usage);
    }
    {
        long long tb_end293 = (tb_tlen(v_pattern) - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end293; v_i++) {
            long long v_t = f_check__builtin_arg(v_c, v_n, v_i);
            const char * v_p = tb_char_at(v_pattern, v_i, -1, 378);
            _Bool v_ok = 1;
            if (({ const char *tb_s294 = v_p; ((unsigned char)tb_s294[0] == 116 && tb_s294[1] == 0); })) {
                v_ok = (v_t == 4LL);
            } else if (({ const char *tb_s295 = v_p; ((unsigned char)tb_s295[0] == 105 && tb_s295[1] == 0); })) {
                v_ok = (v_t == 1LL);
            } else if (({ const char *tb_s296 = v_p; ((unsigned char)tb_s296[0] == 110 && tb_s296[1] == 0); })) {
                v_ok = f_check__is_number(v_t);
            } else if (({ const char *tb_s297 = v_p; ((unsigned char)tb_s297[0] == 76 && tb_s297[1] == 0); })) {
                v_ok = (f_check__is_list(v_c, v_t) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 387))->q_types, v_t, sizeof(tb_o_Type *), 387)), 387))->q_elem == 4LL));
            } else if (({ const char *tb_s298 = v_p; ((unsigned char)tb_s298[0] == 115 && tb_s298[1] == 0); })) {
                v_ok = (f_check__is_list(v_c, v_t) && (((((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 1LL) || (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 2LL)) || (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 4LL)));
            } else if (({ const char *tb_s299 = v_p; ((unsigned char)tb_s299[0] == 120 && tb_s299[1] == 0); })) {
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
    if (({ const char *tb_s300 = v_name; ((unsigned char)tb_s300[0] == 112 && (unsigned char)tb_s300[1] == 114 && (unsigned char)tb_s300[2] == 105 && (unsigned char)tb_s300[3] == 110 && (unsigned char)tb_s300[4] == 116 && tb_s300[5] == 0); })) {
        {
            long long tb_end301 = ((((tb_o_Node *)tb_nn(v_n, 398))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end301; v_i++) {
                long long v_t = f_check__builtin_arg(v_c, v_n, v_i);
            }
        }
        return 0LL;
    }
    if (({ const char *tb_s302 = v_name; ((unsigned char)tb_s302[0] == 108 && (unsigned char)tb_s302[1] == 101 && (unsigned char)tb_s302[2] == 110 && tb_s302[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "x", "len(список, текст или dict)");
        return 1LL;
    }
    if (({ const char *tb_s303 = v_name; ((unsigned char)tb_s303[0] == 114 && (unsigned char)tb_s303[1] == 111 && (unsigned char)tb_s303[2] == 117 && (unsigned char)tb_s303[3] == 110 && (unsigned char)tb_s303[4] == 100 && tb_s303[5] == 0); })) {
        f_check__builtin_args(v_c, v_n, "n", "round(число)");
        return 1LL;
    }
    if (({ const char *tb_s304 = v_name; ((unsigned char)tb_s304[0] == 99 && (unsigned char)tb_s304[1] == 111 && (unsigned char)tb_s304[2] == 100 && (unsigned char)tb_s304[3] == 101 && tb_s304[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "code(\"A\")  →  65");
        return 1LL;
    }
    if (({ const char *tb_s305 = v_name; ((unsigned char)tb_s305[0] == 99 && (unsigned char)tb_s305[1] == 104 && (unsigned char)tb_s305[2] == 97 && (unsigned char)tb_s305[3] == 114 && tb_s305[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "i", "char(66)  →  \"B\"");
        return 4LL;
    }
    if (({ const char *tb_s306 = v_name; ((unsigned char)tb_s306[0] == 99 && (unsigned char)tb_s306[1] == 117 && (unsigned char)tb_s306[2] == 116 && tb_s306[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "tii", "cut(текст, с какого номера, по какой номер)");
        return 4LL;
    }
    if (({ const char *tb_s307 = v_name; ((unsigned char)tb_s307[0] == 116 && (unsigned char)tb_s307[1] == 111 && (unsigned char)tb_s307[2] == 95 && (unsigned char)tb_s307[3] == 105 && (unsigned char)tb_s307[4] == 110 && (unsigned char)tb_s307[5] == 116 && tb_s307[6] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "to_int(\"42\")");
        return 1LL;
    }
    if (({ const char *tb_s308 = v_name; ((unsigned char)tb_s308[0] == 116 && (unsigned char)tb_s308[1] == 111 && (unsigned char)tb_s308[2] == 95 && (unsigned char)tb_s308[3] == 100 && (unsigned char)tb_s308[4] == 101 && (unsigned char)tb_s308[5] == 99 && tb_s308[6] == 0); })) {
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
    if (({ const char *tb_s309 = v_name; ((unsigned char)tb_s309[0] == 105 && (unsigned char)tb_s309[1] == 110 && (unsigned char)tb_s309[2] == 112 && (unsigned char)tb_s309[3] == 117 && (unsigned char)tb_s309[4] == 116 && tb_s309[5] == 0); })) {
        f_check__builtin_args(v_c, v_n, "", "input()");
        return 4LL;
    }
    if (({ const char *tb_s310 = v_name; ((unsigned char)tb_s310[0] == 97 && (unsigned char)tb_s310[1] == 114 && (unsigned char)tb_s310[2] == 103 && (unsigned char)tb_s310[3] == 115 && tb_s310[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "", "args()");
        return f_check__list_of(v_c, 4LL);
    }
    if (({ const char *tb_s311 = v_name; ((unsigned char)tb_s311[0] == 115 && (unsigned char)tb_s311[1] == 112 && (unsigned char)tb_s311[2] == 108 && (unsigned char)tb_s311[3] == 105 && (unsigned char)tb_s311[4] == 116 && tb_s311[5] == 0); })) {
        f_check__builtin_args(v_c, v_n, "tt", "split(текст, разделитель)");
        return f_check__list_of(v_c, 4LL);
    }
    if (({ const char *tb_s312 = v_name; ((unsigned char)tb_s312[0] == 106 && (unsigned char)tb_s312[1] == 111 && (unsigned char)tb_s312[2] == 105 && (unsigned char)tb_s312[3] == 110 && tb_s312[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "Lt", "join(список text, разделитель)");
        return 4LL;
    }
    if (({ const char *tb_s313 = v_name; ((unsigned char)tb_s313[0] == 102 && (unsigned char)tb_s313[1] == 105 && (unsigned char)tb_s313[2] == 110 && (unsigned char)tb_s313[3] == 100 && tb_s313[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "tt", "find(текст, что ищем)  →  номер или -1");
        return 1LL;
    }
    if (({ const char *tb_s314 = v_name; ((unsigned char)tb_s314[0] == 114 && (unsigned char)tb_s314[1] == 101 && (unsigned char)tb_s314[2] == 112 && (unsigned char)tb_s314[3] == 108 && (unsigned char)tb_s314[4] == 97 && (unsigned char)tb_s314[5] == 99 && (unsigned char)tb_s314[6] == 101 && tb_s314[7] == 0); })) {
        f_check__builtin_args(v_c, v_n, "ttt", "replace(текст, что, на что)");
        return 4LL;
    }
    if (((({ const char *tb_s315 = v_name; ((unsigned char)tb_s315[0] == 117 && (unsigned char)tb_s315[1] == 112 && (unsigned char)tb_s315[2] == 112 && (unsigned char)tb_s315[3] == 101 && (unsigned char)tb_s315[4] == 114 && tb_s315[5] == 0); }) || ({ const char *tb_s316 = v_name; ((unsigned char)tb_s316[0] == 108 && (unsigned char)tb_s316[1] == 111 && (unsigned char)tb_s316[2] == 119 && (unsigned char)tb_s316[3] == 101 && (unsigned char)tb_s316[4] == 114 && tb_s316[5] == 0); })) || ({ const char *tb_s317 = v_name; ((unsigned char)tb_s317[0] == 116 && (unsigned char)tb_s317[1] == 114 && (unsigned char)tb_s317[2] == 105 && (unsigned char)tb_s317[3] == 109 && tb_s317[4] == 0); }))) {
        f_check__builtin_args(v_c, v_n, "t", "upper(текст)");
        return 4LL;
    }
    if (((strcmp(v_name, "starts_with") == 0) || (strcmp(v_name, "ends_with") == 0))) {
        f_check__builtin_args(v_c, v_n, "tt", "starts_with(текст, начало)");
        return 3LL;
    }
    if (({ const char *tb_s318 = v_name; ((unsigned char)tb_s318[0] == 115 && (unsigned char)tb_s318[1] == 111 && (unsigned char)tb_s318[2] == 114 && (unsigned char)tb_s318[3] == 116 && tb_s318[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "s", "sort(список int, dec или text)");
        return 0LL;
    }
    if (({ const char *tb_s319 = v_name; ((unsigned char)tb_s319[0] == 114 && (unsigned char)tb_s319[1] == 117 && (unsigned char)tb_s319[2] == 110 && tb_s319[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "run(\"команда терминала\")  →  код выхода int");
        return 1LL;
    }
    if (({ const char *tb_s320 = v_name; ((unsigned char)tb_s320[0] == 101 && (unsigned char)tb_s320[1] == 120 && (unsigned char)tb_s320[2] == 105 && (unsigned char)tb_s320[3] == 116 && tb_s320[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "i", "exit(код)");
        return 0LL;
    }
    if ((strcmp(v_name, "file_exists") == 0)) {
        f_check__builtin_args(v_c, v_n, "t", "file_exists(\"путь\")");
        return 3LL;
    }
    if (({ const char *tb_s321 = v_name; ((unsigned char)tb_s321[0] == 97 && (unsigned char)tb_s321[1] == 98 && (unsigned char)tb_s321[2] == 115 && tb_s321[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "n", "abs(число)");
        return ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 466))->q_kids, 0LL, sizeof(tb_o_Node *), 466)), 466))->q_ty;
    }
    if (({ const char *tb_s322 = v_name; ((unsigned char)tb_s322[0] == 115 && (unsigned char)tb_s322[1] == 113 && (unsigned char)tb_s322[2] == 114 && (unsigned char)tb_s322[3] == 116 && tb_s322[4] == 0); })) {
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
        long long tb_end323 = v_np;
        for (long long v_j = 1LL; v_j <= tb_end323; v_j++) {
            { tb_o_Node * tb_v324 = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *))); *(tb_o_Node * *)tb_grow(v_ordered) = tb_v324; }
            { _Bool tb_v325 = 0; tb_badd(v_taken, tb_v325); }
        }
    }
    _Bool v_named = 0;
    {
        long long tb_end326 = ((((tb_o_Node *)tb_nn(v_call, 488))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end326; v_i++) {
            tb_o_Node * v_arg = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_call, 489))->q_kids, v_i, sizeof(tb_o_Node *), 489));
            long long v_slot = v_i;
            if (({ const char *tb_s327 = ((tb_o_Node *)tb_nn(v_arg, 491))->q_name; !(tb_s327[0] == 0); })) {
                v_named = 1;
                v_slot = (-1LL);
                {
                    long long tb_end328 = (v_np - 1LL);
                    for (long long v_j = 0LL; v_j <= tb_end328; v_j++) {
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
            { tb_o_Node * tb_v329 = v_arg; tb_o_Node * *tb_p329 = &((*(tb_o_Node * *)tb_at(v_ordered, v_slot, sizeof(tb_o_Node *), 503))); *tb_p329 = tb_v329; }
            { _Bool tb_v330 = 1; tb_bset(v_taken, v_slot, tb_v330, 504); }
        }
    }
    { tb_list * tb_v331 = v_ordered; tb_list * *tb_p331 = &(((tb_o_Node *)tb_nn(v_call, 506))->q_kids); *tb_p331 = tb_v331; }
}

static void f_check__need_place_for(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_why, _Bool v_for_link) {
    if ((({ const char *tb_s332 = ((tb_o_Node *)tb_nn(v_n, 510))->q_kind; ((unsigned char)tb_s332[0] == 105 && (unsigned char)tb_s332[1] == 110 && (unsigned char)tb_s332[2] == 100 && (unsigned char)tb_s332[3] == 101 && (unsigned char)tb_s332[4] == 120 && tb_s332[5] == 0); }) && f_check__is_dict(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 510))->q_kids, 0LL, sizeof(tb_o_Node *), 510)), 510))->q_ty))) {
        if (v_for_link) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 512))->q_line, "ярлык на значение в dict нельзя: dict может переложить значения", "");
        }
        return;
    }
    if (({ const char *tb_s333 = ((tb_o_Node *)tb_nn(v_n, 514))->q_kind; ((unsigned char)tb_s333[0] == 110 && (unsigned char)tb_s333[1] == 97 && (unsigned char)tb_s333[2] == 109 && (unsigned char)tb_s333[3] == 101 && tb_s333[4] == 0); })) {
        long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 515))->q_name);
        if (((v_v >= 0LL) && ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 516))->q_vars, v_v, sizeof(tb_o_Var *), 516)), 516))->q_is_const)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 517))->q_line, "это const или переменная цикла, её нельзя менять", ((tb_o_Node *)tb_nn(v_n, 517))->q_name);
        }
        return;
    }
    if (({ const char *tb_s334 = ((tb_o_Node *)tb_nn(v_n, 519))->q_kind; ((unsigned char)tb_s334[0] == 102 && (unsigned char)tb_s334[1] == 105 && (unsigned char)tb_s334[2] == 101 && (unsigned char)tb_s334[3] == 108 && (unsigned char)tb_s334[4] == 100 && tb_s334[5] == 0); })) {
        return;
    }
    if ((((({ const char *tb_s335 = ((tb_o_Node *)tb_nn(v_n, 521))->q_kind; ((unsigned char)tb_s335[0] == 105 && (unsigned char)tb_s335[1] == 110 && (unsigned char)tb_s335[2] == 100 && (unsigned char)tb_s335[3] == 101 && (unsigned char)tb_s335[4] == 120 && tb_s335[5] == 0); }) && f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 521))->q_kids, 0LL, sizeof(tb_o_Node *), 521)), 521))->q_ty)) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 521))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 521))->q_kids, 0LL, sizeof(tb_o_Node *), 521)), 521))->q_ty, sizeof(tb_o_Type *), 521)), 521))->q_elem == 3LL)) && v_for_link)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 522))->q_line, "ярлык на элемент list bool нельзя: там каждый элемент — один бит, у бита нет своего адреса", "");
    }
    if (((({ const char *tb_s336 = ((tb_o_Node *)tb_nn(v_n, 523))->q_kind; ((unsigned char)tb_s336[0] == 105 && (unsigned char)tb_s336[1] == 110 && (unsigned char)tb_s336[2] == 100 && (unsigned char)tb_s336[3] == 101 && (unsigned char)tb_s336[4] == 120 && tb_s336[5] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 523))->q_kids, 0LL, sizeof(tb_o_Node *), 523)), 523))->q_ty)) && v_for_link)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 524))->q_line, "ярлык на элемент списка колонок нельзя: возьми ярлык на качество, например pts[i].x", "");
    }
    if ((({ const char *tb_s337 = ((tb_o_Node *)tb_nn(v_n, 525))->q_kind; ((unsigned char)tb_s337[0] == 105 && (unsigned char)tb_s337[1] == 110 && (unsigned char)tb_s337[2] == 100 && (unsigned char)tb_s337[3] == 101 && (unsigned char)tb_s337[4] == 120 && tb_s337[5] == 0); }) && f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 525))->q_kids, 0LL, sizeof(tb_o_Node *), 525)), 525))->q_ty))) {
        return;
    }
    if (({ const char *tb_s338 = ((tb_o_Node *)tb_nn(v_n, 527))->q_kind; ((unsigned char)tb_s338[0] == 105 && (unsigned char)tb_s338[1] == 110 && (unsigned char)tb_s338[2] == 100 && (unsigned char)tb_s338[3] == 101 && (unsigned char)tb_s338[4] == 120 && tb_s338[5] == 0); })) {
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
        long long tb_end339 = ((((tb_o_Node *)tb_nn(v_n, 536))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end339; v_i++) {
            if (({ const char *tb_s340 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 537))->q_kids, v_i, sizeof(tb_o_Node *), 537)), 537))->q_kind; ((unsigned char)tb_s340[0] == 109 && (unsigned char)tb_s340[1] == 105 && (unsigned char)tb_s340[2] == 115 && (unsigned char)tb_s340[3] == 115 && (unsigned char)tb_s340[4] == 105 && (unsigned char)tb_s340[5] == 110 && (unsigned char)tb_s340[6] == 103 && tb_s340[7] == 0); })) {
                continue;
            }
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 539))->q_kids, v_i, sizeof(tb_o_Node *), 539)), 539))->q_kids, 0LL, sizeof(tb_o_Node *), 539));
            long long v_t = f_check__check_value(v_c, v_value);
            if ((!f_check__fits(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 541))->q_kids, v_i, sizeof(tb_o_Node *), 541)), 541))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 542))->q_line, "качество другого типа", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 542))->q_kids, v_i, sizeof(tb_o_Node *), 542)), 542))->q_name);
            }
            { long long tb_v341 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 543))->q_kids, v_i, sizeof(tb_o_Node *), 543)), 543))->q_ty; long long *tb_p341 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 543))->q_kids, v_i, sizeof(tb_o_Node *), 543)), 543))->q_ty); *tb_p341 = tb_v341; }
        }
    }
    { tb_o_Node * tb_v342 = v_o; tb_o_Node * *tb_p342 = &(((tb_o_Node *)tb_nn(v_n, 544))->q_ref); *tb_p342 = tb_v342; }
    { _Bool tb_v343 = 1; _Bool *tb_p343 = &(((tb_o_Node *)tb_nn(v_n, 545))->q_has_ref); *tb_p343 = tb_v343; }
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
    if (({ const char *tb_s344 = ((tb_o_Node *)tb_nn(v_n, 566))->q_name; ((unsigned char)tb_s344[0] == 109 && (unsigned char)tb_s344[1] == 97 && (unsigned char)tb_s344[2] == 105 && (unsigned char)tb_s344[3] == 110 && tb_s344[4] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 567))->q_line, "main запускается сама, её нельзя вызывать", "");
    }
    if ((((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 568))->q_node, 568))->q_can_fail && (!((tb_o_Node *)tb_nn(v_n, 568))->q_creates))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 569))->q_line, "функция может закончиться ошибкой: напиши  try f(),  f() or запасное  или  f() else err", ((tb_o_Node *)tb_nn(v_n, 569))->q_name);
    }
    { tb_o_Node * tb_v345 = ((tb_o_Func *)tb_nn(v_f, 570))->q_node; tb_o_Node * *tb_p345 = &(((tb_o_Node *)tb_nn(v_n, 570))->q_ref); *tb_p345 = tb_v345; }
    { _Bool tb_v346 = 1; _Bool *tb_p346 = &(((tb_o_Node *)tb_nn(v_n, 571))->q_has_ref); *tb_p346 = tb_v346; }
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 572))->q_node, 572))->q_kids, 0LL, sizeof(tb_o_Node *), 572));
    f_check__order_args(v_c, v_n, v_params, 0);
    {
        long long tb_end347 = ((((tb_o_Node *)tb_nn(v_n, 574))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end347; v_i++) {
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
                    { long long tb_v348 = v_t; long long *tb_p348 = &(((tb_o_Node *)tb_nn(v_p, 585))->q_ty); *tb_p348 = tb_v348; }
                } else if ((((tb_o_Node *)tb_nn(v_p, 586))->q_ty != v_t)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 587))->q_line, "этот ярлык уже был другого типа", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_p, 587))->q_ty));
                }
            } else if ((!f_check__fits(((tb_o_Node *)tb_nn(v_p, 588))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 589))->q_line, "параметр ждёт другой тип", ((tb_o_Node *)tb_nn(v_p, 589))->q_name);
            }
            { long long tb_v349 = ((tb_o_Node *)tb_nn(v_p, 590))->q_ty; long long *tb_p349 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 590))->q_kids, v_i, sizeof(tb_o_Node *), 590)), 590))->q_ty); *tb_p349 = tb_v349; }
        }
    }
    { long long tb_v350 = ((tb_o_Comp *)tb_nn(v_c, 591))->q_current; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 591))->q_call_from) = tb_v350; }
    { long long tb_v351 = v_fi; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 592))->q_call_to) = tb_v351; }
    { _Bool tb_v352 = (((tb_o_Comp *)tb_nn(v_c, 593))->q_loop_depth > 0LL); tb_badd(((tb_o_Comp *)tb_nn(v_c, 593))->q_call_loop, tb_v352); }
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
        { tb_list * tb_v353 = v_fresh_loops; tb_list * *tb_p353 = &(((tb_o_Comp *)tb_nn(v_c, 609))->q_loops); *tb_p353 = tb_v353; }
        { long long tb_v354 = (-1LL); long long *tb_p354 = &(((tb_o_Comp *)tb_nn(v_c, 610))->q_par_base); *tb_p354 = tb_v354; }
        { long long tb_v355 = (-1LL); long long *tb_p355 = &(((tb_o_Comp *)tb_nn(v_c, 611))->q_temp_base); *tb_p355 = tb_v355; }
        { long long tb_v356 = 0LL; long long *tb_p356 = &(((tb_o_Comp *)tb_nn(v_c, 612))->q_piece_depth); *tb_p356 = tb_v356; }
        { long long tb_v357 = (((tb_o_Comp *)tb_nn(v_c, 613))->q_vars)->len; long long *tb_p357 = &(((tb_o_Comp *)tb_nn(v_c, 613))->q_var_base); *tb_p357 = tb_v357; }
        f_check__check_func(v_c, v_fi);
        { long long tb_v358 = v_saved_pieces; long long *tb_p358 = &(((tb_o_Comp *)tb_nn(v_c, 615))->q_piece_depth); *tb_p358 = tb_v358; }
        { tb_list * tb_v359 = v_saved_loop_stack; tb_list * *tb_p359 = &(((tb_o_Comp *)tb_nn(v_c, 616))->q_loops); *tb_p359 = tb_v359; }
        { long long tb_v360 = v_saved_par; long long *tb_p360 = &(((tb_o_Comp *)tb_nn(v_c, 617))->q_par_base); *tb_p360 = tb_v360; }
        { long long tb_v361 = v_saved_temp; long long *tb_p361 = &(((tb_o_Comp *)tb_nn(v_c, 618))->q_temp_base); *tb_p361 = tb_v361; }
        { long long tb_v362 = v_saved_temp_loops; long long *tb_p362 = &(((tb_o_Comp *)tb_nn(v_c, 619))->q_temp_loops); *tb_p362 = tb_v362; }
        { long long tb_v363 = v_saved_base; long long *tb_p363 = &(((tb_o_Comp *)tb_nn(v_c, 620))->q_var_base); *tb_p363 = tb_v363; }
        { const char * tb_v364 = v_saved_mod; const char * *tb_p364 = &(((tb_o_Comp *)tb_nn(v_c, 621))->q_cur_mod); *tb_p364 = tb_v364; }
        { const char * tb_v365 = v_saved_file; const char * *tb_p365 = &(((tb_o_Comp *)tb_nn(v_c, 622))->q_file_name); *tb_p365 = tb_v365; }
        { long long tb_v366 = v_saved_current; long long *tb_p366 = &(((tb_o_Comp *)tb_nn(v_c, 623))->q_current); *tb_p366 = tb_v366; }
        { long long tb_v367 = v_saved_loops; long long *tb_p367 = &(((tb_o_Comp *)tb_nn(v_c, 624))->q_loop_depth); *tb_p367 = tb_v367; }
        f_check__cut_vars(v_c, v_saved_vars);
        { long long tb_v368 = v_saved_depth; long long *tb_p368 = &(((tb_o_Comp *)tb_nn(v_c, 626))->q_depth); *tb_p368 = tb_v368; }
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
    { const char * tb_v369 = "call"; const char * *tb_p369 = &(((tb_o_Node *)tb_nn(v_n, 642))->q_kind); *tb_p369 = tb_v369; }
    tb_list * v_rest = tb_list_new(sizeof(tb_o_Node *));
    {
        long long tb_end370 = ((((tb_o_Node *)tb_nn(v_n, 644))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end370; v_i++) {
            { tb_o_Node * tb_v371 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 645))->q_kids, v_i, sizeof(tb_o_Node *), 645)); *(tb_o_Node * *)tb_grow(v_rest) = tb_v371; }
        }
    }
    { tb_list * tb_v372 = v_rest; tb_list * *tb_p372 = &(((tb_o_Node *)tb_nn(v_n, 646))->q_kids); *tb_p372 = tb_v372; }
    return f_check__check_user_call(v_c, v_n, v_f);
    return v_r;
}

static long long f_check__check_dict_method(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t) {
    long long v_r = 0LL;
    long long v_args = ((((tb_o_Node *)tb_nn(v_n, 650))->q_kids)->len - 1LL);
    tb_o_Type * v_d = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 651))->q_types, v_t, sizeof(tb_o_Type *), 651));
    if ((({ const char *tb_s373 = ((tb_o_Node *)tb_nn(v_n, 652))->q_name; ((unsigned char)tb_s373[0] == 104 && (unsigned char)tb_s373[1] == 97 && (unsigned char)tb_s373[2] == 115 && tb_s373[3] == 0); }) || ({ const char *tb_s374 = ((tb_o_Node *)tb_nn(v_n, 652))->q_name; ((unsigned char)tb_s374[0] == 114 && (unsigned char)tb_s374[1] == 101 && (unsigned char)tb_s374[2] == 109 && (unsigned char)tb_s374[3] == 111 && (unsigned char)tb_s374[4] == 118 && (unsigned char)tb_s374[5] == 101 && tb_s374[6] == 0); }))) {
        if ((v_args != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 654))->q_line, "нужен ровно один ключ", ((tb_o_Node *)tb_nn(v_n, 654))->q_name);
        }
        long long v_k = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 655))->q_kids, 1LL, sizeof(tb_o_Node *), 655)), 655))->q_kids, 0LL, sizeof(tb_o_Node *), 655)));
        if ((v_k != ((tb_o_Type *)tb_nn(v_d, 656))->q_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 657))->q_line, "ключ у этого dict другого типа", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_d, 657))->q_key));
        }
        { long long tb_v375 = ((tb_o_Type *)tb_nn(v_d, 658))->q_key; long long *tb_p375 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 658))->q_kids, 1LL, sizeof(tb_o_Node *), 658)), 658))->q_ty); *tb_p375 = tb_v375; }
        if (({ const char *tb_s376 = ((tb_o_Node *)tb_nn(v_n, 659))->q_name; ((unsigned char)tb_s376[0] == 104 && (unsigned char)tb_s376[1] == 97 && (unsigned char)tb_s376[2] == 115 && tb_s376[3] == 0); })) {
            return 3LL;
        }
        return 0LL;
    }
    if ((({ const char *tb_s377 = ((tb_o_Node *)tb_nn(v_n, 662))->q_name; ((unsigned char)tb_s377[0] == 107 && (unsigned char)tb_s377[1] == 101 && (unsigned char)tb_s377[2] == 121 && (unsigned char)tb_s377[3] == 115 && tb_s377[4] == 0); }) || ({ const char *tb_s378 = ((tb_o_Node *)tb_nn(v_n, 662))->q_name; ((unsigned char)tb_s378[0] == 118 && (unsigned char)tb_s378[1] == 97 && (unsigned char)tb_s378[2] == 108 && (unsigned char)tb_s378[3] == 117 && (unsigned char)tb_s378[4] == 101 && (unsigned char)tb_s378[5] == 115 && tb_s378[6] == 0); }))) {
        if ((v_args != 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 664))->q_line, "пишется без значений", ((tb_o_Node *)tb_nn(v_n, 664))->q_name);
        }
        if (({ const char *tb_s379 = ((tb_o_Node *)tb_nn(v_n, 665))->q_name; ((unsigned char)tb_s379[0] == 107 && (unsigned char)tb_s379[1] == 101 && (unsigned char)tb_s379[2] == 121 && (unsigned char)tb_s379[3] == 115 && tb_s379[4] == 0); })) {
            return f_check__list_of(v_c, ((tb_o_Type *)tb_nn(v_d, 666))->q_key);
        }
        return f_check__list_of(v_c, ((tb_o_Type *)tb_nn(v_d, 667))->q_elem);
    }
    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 668))->q_line, "у dict нет такого метода (есть .has .remove .keys .values)", ((tb_o_Node *)tb_nn(v_n, 668))->q_name);
    return (-1LL);
    return v_r;
}

static long long f_check__check_method(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    tb_o_Node * v_base = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 672))->q_kids, 0LL, sizeof(tb_o_Node *), 672));
    if (((({ const char *tb_s380 = ((tb_o_Node *)tb_nn(v_base, 673))->q_kind; ((unsigned char)tb_s380[0] == 110 && (unsigned char)tb_s380[1] == 97 && (unsigned char)tb_s380[2] == 109 && (unsigned char)tb_s380[3] == 101 && tb_s380[4] == 0); }) && (f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_base, 673))->q_name) < 0LL)) && f_check__is_module(v_c, ((tb_o_Node *)tb_nn(v_base, 673))->q_name))) {
        return f_check__check_module_call(v_c, v_n);
    }
    long long v_t = f_check__check_value(v_c, v_base);
    long long v_args = ((((tb_o_Node *)tb_nn(v_n, 676))->q_kids)->len - 1LL);
    {
        long long tb_end381 = ((((tb_o_Node *)tb_nn(v_n, 677))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end381; v_i++) {
            if (({ const char *tb_s382 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 678))->q_kids, v_i, sizeof(tb_o_Node *), 678)), 678))->q_name; !(tb_s382[0] == 0); })) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 679))->q_line, "у методов аргументы пишутся без имён", "");
            }
        }
    }
    if (((({ const char *tb_s383 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s383[0] == 97 && (unsigned char)tb_s383[1] == 100 && (unsigned char)tb_s383[2] == 100 && tb_s383[3] == 0); }) || ({ const char *tb_s384 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s384[0] == 112 && (unsigned char)tb_s384[1] == 111 && (unsigned char)tb_s384[2] == 112 && tb_s384[3] == 0); })) || ({ const char *tb_s385 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s385[0] == 114 && (unsigned char)tb_s385[1] == 101 && (unsigned char)tb_s385[2] == 109 && (unsigned char)tb_s385[3] == 111 && (unsigned char)tb_s385[4] == 118 && (unsigned char)tb_s385[5] == 101 && tb_s385[6] == 0); }))) {
        f_check__no_outside_change(v_c, v_base);
    }
    if (f_check__is_dict(v_c, v_t)) {
        return f_check__check_dict_method(v_c, v_n, v_t);
    }
    if ((!f_check__is_list(v_c, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 685))->q_line, "у этого значения нет методов: они есть у списков (.add .pop) и dict (.has .remove .keys .values)", ((tb_o_Node *)tb_nn(v_n, 685))->q_name);
    }
    long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 686))->q_types, v_t, sizeof(tb_o_Type *), 686)), 686))->q_elem;
    if (({ const char *tb_s386 = ((tb_o_Node *)tb_nn(v_n, 687))->q_name; ((unsigned char)tb_s386[0] == 97 && (unsigned char)tb_s386[1] == 100 && (unsigned char)tb_s386[2] == 100 && tb_s386[3] == 0); })) {
        if ((v_args != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 689))->q_line, ".add кладёт ровно одно значение", "");
        }
        long long v_v = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 690))->q_kids, 1LL, sizeof(tb_o_Node *), 690)), 690))->q_kids, 0LL, sizeof(tb_o_Node *), 690)));
        if ((!f_check__fits(v_elem, v_v))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 692))->q_line, "в этот список кладут другой тип", f_check__type_name(v_c, v_elem));
        }
        { long long tb_v387 = v_elem; long long *tb_p387 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 693))->q_kids, 1LL, sizeof(tb_o_Node *), 693)), 693))->q_ty); *tb_p387 = tb_v387; }
        return 0LL;
    }
    if (({ const char *tb_s388 = ((tb_o_Node *)tb_nn(v_n, 695))->q_name; ((unsigned char)tb_s388[0] == 112 && (unsigned char)tb_s388[1] == 111 && (unsigned char)tb_s388[2] == 112 && tb_s388[3] == 0); })) {
        if (f_check__is_cols_obj(v_c, v_elem)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 697))->q_line, "у списка колонок (by columns) .pop() пока нет", "");
        }
        if ((v_args != 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 699))->q_line, ".pop() пишется без значений", "");
        }
        return v_elem;
    }
    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 701))->q_line, "у списка нет такого метода (есть .add и .pop)", ((tb_o_Node *)tb_nn(v_n, 701))->q_name);
    return (-1LL);
    return v_r;
}

static long long f_check__check_dict(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    if (((((tb_o_Node *)tb_nn(v_n, 705))->q_kids)->len == 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 706))->q_line, "пустой dict пишется с типом: d = dict text int", "");
    }
    long long v_key = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 707))->q_kids, 0LL, sizeof(tb_o_Node *), 707)));
    long long v_value = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 708))->q_kids, 1LL, sizeof(tb_o_Node *), 708)));
    if (((v_key != 4LL) && (v_key != 1LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 710))->q_line, "ключ у dict бывает text или int", "");
    }
    long long v_i = 2LL;
    while (((v_i + 1LL) < (((tb_o_Node *)tb_nn(v_n, 712))->q_kids)->len)) {
        if ((f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 713))->q_kids, v_i, sizeof(tb_o_Node *), 713))) != v_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 714))->q_kids, v_i, sizeof(tb_o_Node *), 714)), 714))->q_line, "все ключи одного типа", f_check__type_name(v_c, v_key));
        }
        long long v_v = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 715))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 715)));
        if ((!f_check__fits(v_value, v_v))) {
            if (((v_value == 1LL) && (v_v == 2LL))) {
                v_value = 2LL;
            } else {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 720))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 720)), 720))->q_line, "все значения одного типа", f_check__type_name(v_c, v_value));
            }
        }
        { long long tb_v389 = 2LL; long long *tb_p389 = &(v_i); *tb_p389 += tb_v389; }
    }
    return f_check__dict_of(v_c, v_key, v_value);
    return v_r;
}

static _Bool f_check__can_fail_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if (({ const char *tb_s390 = ((tb_o_Node *)tb_nn(v_n, 726))->q_kind; ((unsigned char)tb_s390[0] == 99 && (unsigned char)tb_s390[1] == 97 && (unsigned char)tb_s390[2] == 108 && (unsigned char)tb_s390[3] == 108 && tb_s390[4] == 0); })) {
        if (f_check__is_builtin(((tb_o_Node *)tb_nn(v_n, 727))->q_name)) {
            return f_check__builtin_can_fail(((tb_o_Node *)tb_nn(v_n, 728))->q_name);
        }
        if (((f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 729))->q_name) >= 0LL) || (f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_n, 729))->q_name) >= 0LL))) {
            return 0;
        }
        long long v_f = f_check__find_func(v_c, ((tb_o_Node *)tb_nn(v_n, 731))->q_name);
        return ((v_f >= 0LL) && ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 732))->q_funcs, v_f, sizeof(tb_o_Func *), 732)), 732))->q_node, 732))->q_can_fail);
    }
    if ((((({ const char *tb_s391 = ((tb_o_Node *)tb_nn(v_n, 733))->q_kind; ((unsigned char)tb_s391[0] == 109 && (unsigned char)tb_s391[1] == 101 && (unsigned char)tb_s391[2] == 116 && (unsigned char)tb_s391[3] == 104 && (unsigned char)tb_s391[4] == 111 && (unsigned char)tb_s391[5] == 100 && tb_s391[6] == 0); }) && ({ const char *tb_s392 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 733))->q_kids, 0LL, sizeof(tb_o_Node *), 733)), 733))->q_kind; ((unsigned char)tb_s392[0] == 110 && (unsigned char)tb_s392[1] == 97 && (unsigned char)tb_s392[2] == 109 && (unsigned char)tb_s392[3] == 101 && tb_s392[4] == 0); })) && (f_check__find_var(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 733))->q_kids, 0LL, sizeof(tb_o_Node *), 733)), 733))->q_name) < 0LL)) && f_check__is_module(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 733))->q_kids, 0LL, sizeof(tb_o_Node *), 733)), 733))->q_name))) {
        long long v_f = f_check__find_func_in(v_c, ((tb_o_Node *)tb_nn(v_n, 734))->q_name, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 734))->q_kids, 0LL, sizeof(tb_o_Node *), 734)), 734))->q_name);
        return ((v_f >= 0LL) && ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 735))->q_funcs, v_f, sizeof(tb_o_Func *), 735)), 735))->q_node, 735))->q_can_fail);
    }
    return 0;
    tb_crash(725, "функция can_fail_call дошла до конца без return");
    return 0;
}

static long long f_check__check_handled(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_how) {
    long long v_r = 0LL;
    if ((!f_check__can_fail_call(v_c, v_n))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 741))->q_line, "этот вызов не может закончиться ошибкой, здесь не нужно", v_how);
    }
    { _Bool tb_v393 = 1; _Bool *tb_p393 = &(((tb_o_Node *)tb_nn(v_n, 742))->q_creates); *tb_p393 = tb_v393; }
    return f_check__check_expr(v_c, v_n);
    return v_r;
}

static long long f_check__check_expr(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    long long v_t = (-1LL);
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 747))->q_kind;
    if (({ const char *tb_s394 = v_k; ((unsigned char)tb_s394[0] == 110 && (unsigned char)tb_s394[1] == 117 && (unsigned char)tb_s394[2] == 109 && (unsigned char)tb_s394[3] == 98 && (unsigned char)tb_s394[4] == 101 && (unsigned char)tb_s394[5] == 114 && tb_s394[6] == 0); })) {
        v_t = 1LL;
    } else if (({ const char *tb_s395 = v_k; ((unsigned char)tb_s395[0] == 100 && (unsigned char)tb_s395[1] == 101 && (unsigned char)tb_s395[2] == 99 && (unsigned char)tb_s395[3] == 105 && (unsigned char)tb_s395[4] == 109 && (unsigned char)tb_s395[5] == 97 && (unsigned char)tb_s395[6] == 108 && tb_s395[7] == 0); })) {
        v_t = 2LL;
    } else if (({ const char *tb_s396 = v_k; ((unsigned char)tb_s396[0] == 115 && (unsigned char)tb_s396[1] == 116 && (unsigned char)tb_s396[2] == 114 && (unsigned char)tb_s396[3] == 105 && (unsigned char)tb_s396[4] == 110 && (unsigned char)tb_s396[5] == 103 && tb_s396[6] == 0); })) {
        v_t = 4LL;
    } else if (({ const char *tb_s397 = v_k; ((unsigned char)tb_s397[0] == 98 && (unsigned char)tb_s397[1] == 111 && (unsigned char)tb_s397[2] == 111 && (unsigned char)tb_s397[3] == 108 && tb_s397[4] == 0); })) {
        v_t = 3LL;
    } else if (({ const char *tb_s398 = v_k; ((unsigned char)tb_s398[0] == 110 && (unsigned char)tb_s398[1] == 97 && (unsigned char)tb_s398[2] == 109 && (unsigned char)tb_s398[3] == 101 && tb_s398[4] == 0); })) {
        long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 757))->q_name);
        if ((v_v < 0LL)) {
            if ((f_check__find_func(v_c, ((tb_o_Node *)tb_nn(v_n, 759))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 760))->q_line, "у функции нужны скобки", ((tb_o_Node *)tb_nn(v_n, 760))->q_name);
            }
            if ((f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_n, 761))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 762))->q_line, "чтобы создать obj, нужны скобки", ({ const char *tb_c399_0 = ((tb_o_Node *)tb_nn(v_n, 762))->q_name; long long tb_c399_0n = (long long)strlen(tb_c399_0); const char *tb_c399_1 = "(...)"; long long tb_c399_1n = sizeof("(...)") - 1; char *tb_c399r = tb_mem(tb_c399_0n + tb_c399_1n + 1), *tb_c399rw = tb_c399r; memcpy(tb_c399rw, tb_c399_0, tb_c399_0n); tb_c399rw += tb_c399_0n; memcpy(tb_c399rw, tb_c399_1, tb_c399_1n); tb_c399rw += tb_c399_1n; *tb_c399rw = 0; (const char *)tb_c399r; }));
            }
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 763))->q_line, "нет такой переменной", ((tb_o_Node *)tb_nn(v_n, 763))->q_name);
        }
        if ((f_check__outside_temp(v_c, v_v) && (!f_check__temp_can_see(v_c, ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 764))->q_vars, v_v, sizeof(tb_o_Var *), 764)), 764))->q_type)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 765))->q_line, "внутри temp не видно списков, dict и obj снаружи (кроме списков чисел): создай данные внутри temp или передай числа", ((tb_o_Node *)tb_nn(v_n, 765))->q_name);
        }
        if (f_check__outside_par(v_c, v_v)) {
            if ((((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 767))->q_vars, v_v, sizeof(tb_o_Var *), 767)), 767))->q_is_shared && (!((tb_o_Comp *)tb_nn(v_c, 767))->q_shared_ok))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 768))->q_line, "shared внутри on all cores можно только менять через += или -=, а читать — после цикла", ((tb_o_Node *)tb_nn(v_n, 768))->q_name);
            }
            if ((!(((tb_o_Comp *)tb_nn(v_c, 769))->q_par_vec && ((tb_o_Comp *)tb_nn(v_c, 769))->q_shared_ok))) {
                f_check__capture(v_c, v_v);
            }
        }
        { _Bool tb_v400 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 771))->q_vars, v_v, sizeof(tb_o_Var *), 771)), 771))->q_is_link; _Bool *tb_p400 = &(((tb_o_Node *)tb_nn(v_n, 771))->q_is_link); *tb_p400 = tb_v400; }
        v_t = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 772))->q_vars, v_v, sizeof(tb_o_Var *), 772)), 772))->q_type;
    } else if (({ const char *tb_s401 = v_k; ((unsigned char)tb_s401[0] == 108 && (unsigned char)tb_s401[1] == 105 && (unsigned char)tb_s401[2] == 115 && (unsigned char)tb_s401[3] == 116 && tb_s401[4] == 0); })) {
        v_t = f_check__check_list(v_c, v_n);
    } else if (({ const char *tb_s402 = v_k; ((unsigned char)tb_s402[0] == 100 && (unsigned char)tb_s402[1] == 105 && (unsigned char)tb_s402[2] == 99 && (unsigned char)tb_s402[3] == 116 && tb_s402[4] == 0); })) {
        v_t = f_check__check_dict(v_c, v_n);
    } else if (({ const char *tb_s403 = v_k; ((unsigned char)tb_s403[0] == 116 && (unsigned char)tb_s403[1] == 114 && (unsigned char)tb_s403[2] == 121 && tb_s403[3] == 0); })) {
        if ((((tb_o_Comp *)tb_nn(v_c, 778))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 779))->q_line, ({ const char *tb_c404_0 = "внутри "; long long tb_c404_0n = sizeof("внутри ") - 1; const char *tb_c404_1 = f_check__loop_word(v_c); long long tb_c404_1n = (long long)strlen(tb_c404_1); const char *tb_c404_2 = " try нельзя: ядро не может остановить всю функцию, пиши  or  или  else err"; long long tb_c404_2n = sizeof(" try нельзя: ядро не может остановить всю функцию, пиши  or  или  else err") - 1; char *tb_c404r = tb_mem(tb_c404_0n + tb_c404_1n + tb_c404_2n + 1), *tb_c404rw = tb_c404r; memcpy(tb_c404rw, tb_c404_0, tb_c404_0n); tb_c404rw += tb_c404_0n; memcpy(tb_c404rw, tb_c404_1, tb_c404_1n); tb_c404rw += tb_c404_1n; memcpy(tb_c404rw, tb_c404_2, tb_c404_2n); tb_c404rw += tb_c404_2n; *tb_c404rw = 0; (const char *)tb_c404r; }), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 780))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 781))->q_line, "внутри temp try нельзя: пиши  or  или  else err", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 782))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 783))->q_line, "внутри each ... by N try нельзя: пиши  or  или  else err", "");
        }
        v_t = f_check__check_handled(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 784))->q_kids, 0LL, sizeof(tb_o_Node *), 784)), "try");
    } else if (({ const char *tb_s405 = v_k; ((unsigned char)tb_s405[0] == 101 && (unsigned char)tb_s405[1] == 108 && (unsigned char)tb_s405[2] == 115 && (unsigned char)tb_s405[3] == 101 && (unsigned char)tb_s405[4] == 95 && (unsigned char)tb_s405[5] == 101 && (unsigned char)tb_s405[6] == 114 && (unsigned char)tb_s405[7] == 114 && tb_s405[8] == 0); })) {
        v_t = f_check__check_handled(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 786))->q_kids, 0LL, sizeof(tb_o_Node *), 786)), "else");
        f_check__open_scope(v_c);
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 788))->q_name, 4LL, 1, 0, ((tb_o_Node *)tb_nn(v_n, 788))->q_line);
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 789))->q_kids, 1LL, sizeof(tb_o_Node *), 789)));
        f_check__close_scope(v_c);
    } else if (({ const char *tb_s406 = v_k; ((unsigned char)tb_s406[0] == 99 && (unsigned char)tb_s406[1] == 97 && (unsigned char)tb_s406[2] == 108 && (unsigned char)tb_s406[3] == 108 && tb_s406[4] == 0); })) {
        v_t = f_check__check_call(v_c, v_n);
    } else if (({ const char *tb_s407 = v_k; ((unsigned char)tb_s407[0] == 109 && (unsigned char)tb_s407[1] == 101 && (unsigned char)tb_s407[2] == 116 && (unsigned char)tb_s407[3] == 104 && (unsigned char)tb_s407[4] == 111 && (unsigned char)tb_s407[5] == 100 && tb_s407[6] == 0); })) {
        v_t = f_check__check_method(v_c, v_n);
    } else if (({ const char *tb_s408 = v_k; ((unsigned char)tb_s408[0] == 98 && (unsigned char)tb_s408[1] == 105 && (unsigned char)tb_s408[2] == 110 && (unsigned char)tb_s408[3] == 111 && (unsigned char)tb_s408[4] == 112 && tb_s408[5] == 0); })) {
        v_t = f_check__check_binop(v_c, v_n);
    } else if (({ const char *tb_s409 = v_k; ((unsigned char)tb_s409[0] == 117 && (unsigned char)tb_s409[1] == 110 && (unsigned char)tb_s409[2] == 97 && (unsigned char)tb_s409[3] == 114 && (unsigned char)tb_s409[4] == 121 && tb_s409[5] == 0); })) {
        long long v_inner = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 798))->q_kids, 0LL, sizeof(tb_o_Node *), 798)));
        if (({ const char *tb_s410 = ((tb_o_Node *)tb_nn(v_n, 799))->q_value; ((unsigned char)tb_s410[0] == 110 && (unsigned char)tb_s410[1] == 111 && (unsigned char)tb_s410[2] == 116 && tb_s410[3] == 0); })) {
            if ((v_inner != 3LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 801))->q_line, "not работает только с true и false", "");
            }
            v_t = 3LL;
        } else {
            if ((!f_check__is_number(v_inner))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 805))->q_line, "минус ставится только перед числом", "");
            }
            v_t = v_inner;
        }
    } else if ((({ const char *tb_s411 = v_k; ((unsigned char)tb_s411[0] == 105 && (unsigned char)tb_s411[1] == 110 && (unsigned char)tb_s411[2] == 100 && (unsigned char)tb_s411[3] == 101 && (unsigned char)tb_s411[4] == 120 && tb_s411[5] == 0); }) && f_check__is_dict(v_c, f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 807))->q_kids, 0LL, sizeof(tb_o_Node *), 807)))))) {
        tb_o_Type * v_base = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 808))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 808))->q_kids, 0LL, sizeof(tb_o_Node *), 808)), 808))->q_ty, sizeof(tb_o_Type *), 808));
        if ((f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 809))->q_kids, 1LL, sizeof(tb_o_Node *), 809))) != ((tb_o_Type *)tb_nn(v_base, 809))->q_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 810))->q_line, "ключ у этого dict другого типа", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_base, 810))->q_key));
        }
        v_t = ((tb_o_Type *)tb_nn(v_base, 811))->q_elem;
    } else if (({ const char *tb_s412 = v_k; ((unsigned char)tb_s412[0] == 105 && (unsigned char)tb_s412[1] == 110 && (unsigned char)tb_s412[2] == 100 && (unsigned char)tb_s412[3] == 101 && (unsigned char)tb_s412[4] == 120 && tb_s412[5] == 0); })) {
        long long v_base = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 813))->q_kids, 0LL, sizeof(tb_o_Node *), 813)));
        long long v_index = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 814))->q_kids, 1LL, sizeof(tb_o_Node *), 814)));
        if ((v_index != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 816))->q_line, "номер в списке должен быть int", "");
        }
        if ((v_base == 4LL)) {
            v_t = 4LL;
        } else if (f_check__is_list(v_c, v_base)) {
            v_t = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 820))->q_types, v_base, sizeof(tb_o_Type *), 820)), 820))->q_elem;
        } else {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 822))->q_line, "[номер] берут у списка или текста", "");
        }
    } else if (((({ const char *tb_s413 = v_k; ((unsigned char)tb_s413[0] == 102 && (unsigned char)tb_s413[1] == 105 && (unsigned char)tb_s413[2] == 101 && (unsigned char)tb_s413[3] == 108 && (unsigned char)tb_s413[4] == 100 && tb_s413[5] == 0); }) && ({ const char *tb_s414 = ((tb_o_Node *)tb_nn(v_n, 823))->q_name; ((unsigned char)tb_s414[0] == 119 && (unsigned char)tb_s414[1] == 97 && (unsigned char)tb_s414[2] == 105 && (unsigned char)tb_s414[3] == 116 && tb_s414[4] == 0); })) && ({ const char *tb_s415 = f_check__kind_of(v_c, f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 823))->q_kids, 0LL, sizeof(tb_o_Node *), 823)))); ((unsigned char)tb_s415[0] == 116 && (unsigned char)tb_s415[1] == 97 && (unsigned char)tb_s415[2] == 115 && (unsigned char)tb_s415[3] == 107 && tb_s415[4] == 0); }))) {
        if ((((tb_o_Comp *)tb_nn(v_c, 824))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 825))->q_line, ({ const char *tb_c416_0 = "внутри "; long long tb_c416_0n = sizeof("внутри ") - 1; const char *tb_c416_1 = f_check__loop_word(v_c); long long tb_c416_1n = (long long)strlen(tb_c416_1); const char *tb_c416_2 = " .wait нельзя: забери готовое до цикла или после"; long long tb_c416_2n = sizeof(" .wait нельзя: забери готовое до цикла или после") - 1; char *tb_c416r = tb_mem(tb_c416_0n + tb_c416_1n + tb_c416_2n + 1), *tb_c416rw = tb_c416r; memcpy(tb_c416rw, tb_c416_0, tb_c416_0n); tb_c416rw += tb_c416_0n; memcpy(tb_c416rw, tb_c416_1, tb_c416_1n); tb_c416rw += tb_c416_1n; memcpy(tb_c416rw, tb_c416_2, tb_c416_2n); tb_c416rw += tb_c416_2n; *tb_c416rw = 0; (const char *)tb_c416r; }), "");
        }
        v_t = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 826))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 826))->q_kids, 0LL, sizeof(tb_o_Node *), 826)), 826))->q_ty, sizeof(tb_o_Type *), 826)), 826))->q_elem;
    } else if ((strcmp(v_k, "background") == 0)) {
        v_t = f_check__check_background(v_c, v_n);
    } else if (({ const char *tb_s417 = v_k; ((unsigned char)tb_s417[0] == 102 && (unsigned char)tb_s417[1] == 105 && (unsigned char)tb_s417[2] == 101 && (unsigned char)tb_s417[3] == 108 && (unsigned char)tb_s417[4] == 100 && tb_s417[5] == 0); })) {
        long long v_base = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 830))->q_kids, 0LL, sizeof(tb_o_Node *), 830)));
        if (({ const char *tb_s418 = f_check__kind_of(v_c, v_base); !((unsigned char)tb_s418[0] == 111 && (unsigned char)tb_s418[1] == 98 && (unsigned char)tb_s418[2] == 106 && tb_s418[3] == 0); })) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 832))->q_line, "точка с качеством бывает только у obj", ((tb_o_Node *)tb_nn(v_n, 832))->q_name);
        }
        tb_o_Node * v_o = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 833))->q_types, v_base, sizeof(tb_o_Type *), 833)), 833))->q_obj_node;
        long long v_field = f_check__find_field(v_o, ((tb_o_Node *)tb_nn(v_n, 834))->q_name);
        if ((v_field < 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 836))->q_line, "у этого obj нет такого качества", ((tb_o_Node *)tb_nn(v_n, 836))->q_name);
        }
        v_t = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 837))->q_kids, v_field, sizeof(tb_o_Node *), 837)), 837))->q_ty;
        { tb_o_Node * tb_v419 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 838))->q_kids, v_field, sizeof(tb_o_Node *), 838)); *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 838))->q_acc_field) = tb_v419; }
        { long long tb_v420 = ((tb_o_Comp *)tb_nn(v_c, 839))->q_current; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 839))->q_acc_func) = tb_v420; }
        { _Bool tb_v421 = (((tb_o_Comp *)tb_nn(v_c, 840))->q_loop_depth > 0LL); tb_badd(((tb_o_Comp *)tb_nn(v_c, 840))->q_acc_loop, tb_v421); }
    }
    { long long tb_v422 = v_t; long long *tb_p422 = &(((tb_o_Node *)tb_nn(v_n, 841))->q_ty); *tb_p422 = tb_v422; }
    if (({ const char *tb_s423 = v_k; ((unsigned char)tb_s423[0] == 105 && (unsigned char)tb_s423[1] == 110 && (unsigned char)tb_s423[2] == 100 && (unsigned char)tb_s423[3] == 101 && (unsigned char)tb_s423[4] == 120 && tb_s423[5] == 0); })) {
        f_check__note_prefetch(v_c, v_n);
    }
    return v_t;
    return v_r;
}

static void f_check__note_prefetch(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if (((((tb_o_Comp *)tb_nn(v_c, 850))->q_loops)->len == 0LL)) {
        return;
    }
    tb_o_Node * v_loop = (*(tb_o_Node * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 852))->q_loops, ((((tb_o_Comp *)tb_nn(v_c, 852))->q_loops)->len - 1LL), sizeof(tb_o_Node *), 852));
    if (((((tb_o_Node *)tb_nn(v_loop, 853))->q_is_par || ((tb_o_Node *)tb_nn(v_loop, 853))->q_by_vec) || ((tb_o_Node *)tb_nn(v_loop, 853))->q_by_piece)) {
        return;
    }
    tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 855))->q_kids, 0LL, sizeof(tb_o_Node *), 855));
    if ((({ const char *tb_s424 = ((tb_o_Node *)tb_nn(v_a, 856))->q_kind; !((unsigned char)tb_s424[0] == 110 && (unsigned char)tb_s424[1] == 97 && (unsigned char)tb_s424[2] == 109 && (unsigned char)tb_s424[3] == 101 && tb_s424[4] == 0); }) || (!f_check__declared_before(v_c, ((tb_o_Node *)tb_nn(v_a, 856))->q_name, ((tb_o_Node *)tb_nn(v_loop, 856))->q_name)))) {
        return;
    }
    if ((!((f_check__is_list(v_c, ((tb_o_Node *)tb_nn(v_a, 858))->q_ty) && (!f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_a, 858))->q_ty))) || f_check__is_dict(v_c, ((tb_o_Node *)tb_nn(v_a, 858))->q_ty)))) {
        return;
    }
    tb_o_Node * v_i = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 860))->q_kids, 1LL, sizeof(tb_o_Node *), 860));
    tb_o_Node * v_plan = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    if (({ const char *tb_s425 = ((tb_o_Node *)tb_nn(v_loop, 862))->q_kind; ((unsigned char)tb_s425[0] == 101 && (unsigned char)tb_s425[1] == 97 && (unsigned char)tb_s425[2] == 99 && (unsigned char)tb_s425[3] == 104 && tb_s425[4] == 0); })) {
        if (((!f_check__is_name_of(v_i, ((tb_o_Node *)tb_nn(v_loop, 863))->q_name)) || ((!f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 863))->q_kids, 0LL, sizeof(tb_o_Node *), 863)), 863))->q_ty)) && (!f_check__is_dict(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 863))->q_kids, 0LL, sizeof(tb_o_Node *), 863)), 863))->q_ty))))) {
            return;
        }
    } else {
        if ((({ const char *tb_s426 = ((tb_o_Node *)tb_nn(v_i, 866))->q_kind; !((unsigned char)tb_s426[0] == 105 && (unsigned char)tb_s426[1] == 110 && (unsigned char)tb_s426[2] == 100 && (unsigned char)tb_s426[3] == 101 && (unsigned char)tb_s426[4] == 120 && tb_s426[5] == 0); }) || (!f_check__is_name_of((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_i, 866))->q_kids, 1LL, sizeof(tb_o_Node *), 866)), ((tb_o_Node *)tb_nn(v_loop, 866))->q_name)))) {
            return;
        }
        v_plan = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_i, 868))->q_kids, 0LL, sizeof(tb_o_Node *), 868));
        if ((((({ const char *tb_s427 = ((tb_o_Node *)tb_nn(v_plan, 869))->q_kind; !((unsigned char)tb_s427[0] == 110 && (unsigned char)tb_s427[1] == 97 && (unsigned char)tb_s427[2] == 109 && (unsigned char)tb_s427[3] == 101 && tb_s427[4] == 0); }) || (!f_check__declared_before(v_c, ((tb_o_Node *)tb_nn(v_plan, 869))->q_name, ((tb_o_Node *)tb_nn(v_loop, 869))->q_name))) || (!f_check__is_list(v_c, ((tb_o_Node *)tb_nn(v_plan, 869))->q_ty))) || f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_plan, 869))->q_ty))) {
            return;
        }
    }
    {
        long long tb_end428 = ((((tb_o_Node *)tb_nn(v_loop, 871))->q_pre_bases)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end428; v_j++) {
            if (((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 872))->q_pre_bases, v_j, sizeof(tb_o_Node *), 872)), 872))->q_name, ((tb_o_Node *)tb_nn(v_a, 872))->q_name) == 0) && (strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 872))->q_pre_idx, v_j, sizeof(tb_o_Node *), 872)), 872))->q_name, ((tb_o_Node *)tb_nn(v_plan, 872))->q_name) == 0))) {
                return;
            }
        }
    }
    { tb_o_Node * tb_v429 = v_a; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_loop, 874))->q_pre_bases) = tb_v429; }
    { tb_o_Node * tb_v430 = v_plan; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_loop, 875))->q_pre_idx) = tb_v430; }
}

static _Bool f_check__declared_before(tb_o_Comp * v_c, const char * v_name, const char * v_loop_var) {
    long long v_a = f_check__find_var(v_c, v_name);
    long long v_b = f_check__find_var(v_c, v_loop_var);
    return (((v_a >= 0LL) && (v_b >= 0LL)) && (v_a < v_b));
    tb_crash(878, "функция declared_before дошла до конца без return");
    return 0;
}

static void f_check__need_bool(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if ((f_check__check_value(v_c, v_n) != 3LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 885))->q_line, "условие должно быть true или false (например a > 3)", "");
    }
}

static void f_check__need_int(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_what) {
    if ((f_check__check_value(v_c, v_n) != 1LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 889))->q_line, "здесь нужно целое число int", v_what);
    }
}

static void f_check__check_assign(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_declared = (-1LL);
    if (((tb_o_Node *)tb_nn(v_n, 895))->q_has_tsyn) {
        v_declared = f_check__resolve_type(v_c, ((tb_o_Node *)tb_nn(v_n, 896))->q_tsyn);
    }
    long long v_t = (-1LL);
    if (((((tb_o_Node *)tb_nn(v_n, 898))->q_kids)->len == 1LL)) {
        v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 899))->q_kids, 0LL, sizeof(tb_o_Node *), 899)));
    }
    long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 900))->q_name);
    if (((tb_o_Node *)tb_nn(v_n, 901))->q_is_shared) {
        if ((v_v >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 903))->q_line, "shared пишется только у новой переменной", ((tb_o_Node *)tb_nn(v_n, 903))->q_name);
        }
        if (((((((v_declared != 1LL) && (v_declared != 2LL)) && (v_declared != 4LL)) && (v_t != 1LL)) && (v_t != 2LL)) && (v_t != 4LL))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 905))->q_line, "shared бывает int, dec или text: shared y = int 0, shared out = \"\"", ((tb_o_Node *)tb_nn(v_n, 905))->q_name);
        }
    }
    if ((v_v < 0LL)) {
        long long v_type = v_t;
        if ((v_declared >= 0LL)) {
            if (((v_t >= 0LL) && (!f_check__fits(v_declared, v_t)))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 910))->q_line, "значение не того типа, что написан", f_check__type_name(v_c, v_declared));
            }
            v_type = v_declared;
        }
        { _Bool tb_v431 = 1; _Bool *tb_p431 = &(((tb_o_Node *)tb_nn(v_n, 912))->q_creates); *tb_p431 = tb_v431; }
        { long long tb_v432 = v_type; long long *tb_p432 = &(((tb_o_Node *)tb_nn(v_n, 913))->q_ty); *tb_p432 = tb_v432; }
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 914))->q_name, v_type, ((tb_o_Node *)tb_nn(v_n, 914))->q_is_const, 0, ((tb_o_Node *)tb_nn(v_n, 914))->q_line);
        { _Bool tb_v433 = ((tb_o_Node *)tb_nn(v_n, 915))->q_is_shared; _Bool *tb_p433 = &(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 915))->q_vars, ((((tb_o_Comp *)tb_nn(v_c, 915))->q_vars)->len - 1LL), sizeof(tb_o_Var *), 915)), 915))->q_is_shared); *tb_p433 = tb_v433; }
        return;
    }
    if ((f_check__outside_temp(v_c, v_v) && (!f_check__is_plain(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 917))->q_vars, v_v, sizeof(tb_o_Var *), 917)), 917))->q_type)))) {
        f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_n, 918))->q_line, ((tb_o_Node *)tb_nn(v_n, 918))->q_name);
    }
    if (f_check__outside_par(v_c, v_v)) {
        if (((tb_o_Comp *)tb_nn(v_c, 920))->q_par_vec) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 921))->q_line, "внутри by vec нельзя менять переменную снаружи: копи её через += или -=", ((tb_o_Node *)tb_nn(v_n, 921))->q_name);
        }
        if (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 922))->q_vars, v_v, sizeof(tb_o_Var *), 922)), 922))->q_is_shared) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 923))->q_line, "shared внутри on all cores меняется только через += или -=", ((tb_o_Node *)tb_nn(v_n, 923))->q_name);
        }
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 924))->q_line, ({ const char *tb_c434_0 = "внутри "; long long tb_c434_0n = sizeof("внутри ") - 1; const char *tb_c434_1 = f_check__loop_word(v_c); long long tb_c434_1n = (long long)strlen(tb_c434_1); const char *tb_c434_2 = " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="; long long tb_c434_2n = sizeof(" нельзя менять переменную снаружи: сделай её shared и меняй через += или -=") - 1; char *tb_c434r = tb_mem(tb_c434_0n + tb_c434_1n + tb_c434_2n + 1), *tb_c434rw = tb_c434r; memcpy(tb_c434rw, tb_c434_0, tb_c434_0n); tb_c434rw += tb_c434_0n; memcpy(tb_c434rw, tb_c434_1, tb_c434_1n); tb_c434rw += tb_c434_1n; memcpy(tb_c434rw, tb_c434_2, tb_c434_2n); tb_c434rw += tb_c434_2n; *tb_c434rw = 0; (const char *)tb_c434r; }), ((tb_o_Node *)tb_nn(v_n, 924))->q_name);
    }
    if ((((tb_o_Node *)tb_nn(v_n, 925))->q_is_const || (v_declared >= 0LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 926))->q_line, "переменная уже есть: тип и const пишутся только в первый раз", ((tb_o_Node *)tb_nn(v_n, 926))->q_name);
    }
    if (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 927))->q_vars, v_v, sizeof(tb_o_Var *), 927)), 927))->q_is_const) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 928))->q_line, "это const или переменная цикла, её нельзя менять", ((tb_o_Node *)tb_nn(v_n, 928))->q_name);
    }
    if ((!f_check__fits(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 929))->q_vars, v_v, sizeof(tb_o_Var *), 929)), 929))->q_type, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 930))->q_line, "переменная другого типа", f_check__type_name(v_c, ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 930))->q_vars, v_v, sizeof(tb_o_Var *), 930)), 930))->q_type));
    }
    { _Bool tb_v435 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 931))->q_vars, v_v, sizeof(tb_o_Var *), 931)), 931))->q_is_link; _Bool *tb_p435 = &(((tb_o_Node *)tb_nn(v_n, 931))->q_is_link); *tb_p435 = tb_v435; }
    { long long tb_v436 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 932))->q_vars, v_v, sizeof(tb_o_Var *), 932)), 932))->q_type; long long *tb_p436 = &(((tb_o_Node *)tb_nn(v_n, 932))->q_ty); *tb_p436 = tb_v436; }
}

static void f_check__check_set(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 935))->q_kids, 0LL, sizeof(tb_o_Node *), 935));
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 936))->q_value;
    { _Bool tb_v437 = (({ const char *tb_s438 = ((tb_o_Node *)tb_nn(v_target, 937))->q_kind; ((unsigned char)tb_s438[0] == 110 && (unsigned char)tb_s438[1] == 97 && (unsigned char)tb_s438[2] == 109 && (unsigned char)tb_s438[3] == 101 && tb_s438[4] == 0); }) && (({ const char *tb_s439 = v_op; ((unsigned char)tb_s439[0] == 43 && (unsigned char)tb_s439[1] == 61 && tb_s439[2] == 0); }) || ({ const char *tb_s440 = v_op; ((unsigned char)tb_s440[0] == 45 && (unsigned char)tb_s440[1] == 61 && tb_s440[2] == 0); }))); _Bool *tb_p437 = &(((tb_o_Comp *)tb_nn(v_c, 937))->q_shared_ok); *tb_p437 = tb_v437; }
    long long v_to_ty = f_check__check_value(v_c, v_target);
    { _Bool tb_v441 = 0; _Bool *tb_p441 = &(((tb_o_Comp *)tb_nn(v_c, 939))->q_shared_ok); *tb_p441 = tb_v441; }
    f_check__need_place(v_c, v_target, "менять можно только переменную, u.age или nums[i]");
    long long v_r = f_check__root_var(v_c, v_target);
    if (((f_check__outside_temp(v_c, v_r) && ({ const char *tb_s442 = ((tb_o_Node *)tb_nn(v_target, 942))->q_kind; ((unsigned char)tb_s442[0] == 110 && (unsigned char)tb_s442[1] == 97 && (unsigned char)tb_s442[2] == 109 && (unsigned char)tb_s442[3] == 101 && tb_s442[4] == 0); })) && (!f_check__is_plain(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 942))->q_vars, v_r, sizeof(tb_o_Var *), 942)), 942))->q_type)))) {
        f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_n, 943))->q_line, ((tb_o_Node *)tb_nn(v_target, 943))->q_name);
    }
    if ((f_check__outside_par(v_c, v_r) && ((tb_o_Comp *)tb_nn(v_c, 944))->q_par_vec)) {
        if (({ const char *tb_s443 = ((tb_o_Node *)tb_nn(v_target, 945))->q_kind; !((unsigned char)tb_s443[0] == 110 && (unsigned char)tb_s443[1] == 97 && (unsigned char)tb_s443[2] == 109 && (unsigned char)tb_s443[3] == 101 && tb_s443[4] == 0); })) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 946))->q_line, "внутри by vec нельзя менять списки и obj снаружи цикла", "");
        }
        tb_o_Var * v_var = (*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 947))->q_vars, v_r, sizeof(tb_o_Var *), 947));
        if (((({ const char *tb_s444 = v_op; !((unsigned char)tb_s444[0] == 43 && (unsigned char)tb_s444[1] == 61 && tb_s444[2] == 0); }) && ({ const char *tb_s445 = v_op; !((unsigned char)tb_s445[0] == 45 && (unsigned char)tb_s445[1] == 61 && tb_s445[2] == 0); })) || (!f_check__is_number(((tb_o_Var *)tb_nn(v_var, 948))->q_type)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 949))->q_line, "внутри by vec число снаружи можно только копить через += или -=", ((tb_o_Node *)tb_nn(v_target, 949))->q_name);
        }
        _Bool v_found = 0;
        {
            tb_list * tb_l446 = ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 951))->q_par_node, 951))->q_caps;
            for (long long tb_i446 = 0; tb_i446 < tb_l446->len; tb_i446++) {
                tb_o_Var * const v_old = ((tb_o_Var * *)tb_l446->data)[tb_i446];
                if (((strcmp(((tb_o_Var *)tb_nn(v_old, 952))->q_name, ((tb_o_Var *)tb_nn(v_var, 952))->q_name) == 0) && ((tb_o_Var *)tb_nn(v_old, 952))->q_is_shared)) {
                    v_found = 1;
                    if ((strcmp(((tb_o_Var *)tb_nn(v_old, 954))->q_op, v_op) != 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 955))->q_line, "в by vec одну переменную копят одним знаком: или +=, или -=", ((tb_o_Node *)tb_nn(v_target, 955))->q_name);
                    }
                }
            }
        }
        if ((!v_found)) {
            { tb_o_Var * tb_v447 = tb_new_Var(((tb_o_Var *)tb_nn(v_var, 957))->q_name, ((tb_o_Var *)tb_nn(v_var, 957))->q_type, 0, ((tb_o_Var *)tb_nn(v_var, 957))->q_is_link, 0LL, 1, v_op); *(tb_o_Var * *)tb_grow(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 957))->q_par_node, 957))->q_caps) = tb_v447; }
        }
    } else if (f_check__outside_par(v_c, v_r)) {
        if (({ const char *tb_s448 = ((tb_o_Node *)tb_nn(v_target, 959))->q_kind; !((unsigned char)tb_s448[0] == 110 && (unsigned char)tb_s448[1] == 97 && (unsigned char)tb_s448[2] == 109 && (unsigned char)tb_s448[3] == 101 && tb_s448[4] == 0); })) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 960))->q_line, ({ const char *tb_c449_0 = "внутри "; long long tb_c449_0n = sizeof("внутри ") - 1; const char *tb_c449_1 = f_check__loop_word(v_c); long long tb_c449_1n = (long long)strlen(tb_c449_1); const char *tb_c449_2 = " нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)"; long long tb_c449_2n = sizeof(" нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)") - 1; char *tb_c449r = tb_mem(tb_c449_0n + tb_c449_1n + tb_c449_2n + 1), *tb_c449rw = tb_c449r; memcpy(tb_c449rw, tb_c449_0, tb_c449_0n); tb_c449rw += tb_c449_0n; memcpy(tb_c449rw, tb_c449_1, tb_c449_1n); tb_c449rw += tb_c449_1n; memcpy(tb_c449rw, tb_c449_2, tb_c449_2n); tb_c449rw += tb_c449_2n; *tb_c449rw = 0; (const char *)tb_c449r; }), "");
        }
        if ((!((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 961))->q_vars, v_r, sizeof(tb_o_Var *), 961)), 961))->q_is_shared)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 962))->q_line, ({ const char *tb_c450_0 = "внутри "; long long tb_c450_0n = sizeof("внутри ") - 1; const char *tb_c450_1 = f_check__loop_word(v_c); long long tb_c450_1n = (long long)strlen(tb_c450_1); const char *tb_c450_2 = " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="; long long tb_c450_2n = sizeof(" нельзя менять переменную снаружи: сделай её shared и меняй через += или -=") - 1; char *tb_c450r = tb_mem(tb_c450_0n + tb_c450_1n + tb_c450_2n + 1), *tb_c450rw = tb_c450r; memcpy(tb_c450rw, tb_c450_0, tb_c450_0n); tb_c450rw += tb_c450_0n; memcpy(tb_c450rw, tb_c450_1, tb_c450_1n); tb_c450rw += tb_c450_1n; memcpy(tb_c450rw, tb_c450_2, tb_c450_2n); tb_c450rw += tb_c450_2n; *tb_c450rw = 0; (const char *)tb_c450r; }), ((tb_o_Node *)tb_nn(v_target, 962))->q_name);
        }
        if ((({ const char *tb_s451 = v_op; !((unsigned char)tb_s451[0] == 43 && (unsigned char)tb_s451[1] == 61 && tb_s451[2] == 0); }) && ({ const char *tb_s452 = v_op; !((unsigned char)tb_s452[0] == 45 && (unsigned char)tb_s452[1] == 61 && tb_s452[2] == 0); }))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 964))->q_line, "shared внутри on all cores меняется только через += или -=", ((tb_o_Node *)tb_nn(v_target, 964))->q_name);
        }
    }
    long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 965))->q_kids, 1LL, sizeof(tb_o_Node *), 965)));
    { long long tb_v453 = v_to_ty; long long *tb_p453 = &(((tb_o_Node *)tb_nn(v_n, 966))->q_ty); *tb_p453 = tb_v453; }
    if (({ const char *tb_s454 = v_op; ((unsigned char)tb_s454[0] == 61 && tb_s454[1] == 0); })) {
        if ((!f_check__fits(v_to_ty, v_t))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 969))->q_line, "здесь лежит другой тип", f_check__type_name(v_c, v_to_ty));
        }
        return;
    }
    if (((v_to_ty == 4LL) && ({ const char *tb_s455 = v_op; ((unsigned char)tb_s455[0] == 43 && (unsigned char)tb_s455[1] == 61 && tb_s455[2] == 0); }))) {
        if ((!f_check__is_scalar(v_t))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 973))->q_line, "к тексту можно приписать только текст, число или bool", "");
        }
        return;
    }
    if (((!f_check__is_number(v_to_ty)) || (!f_check__is_number(v_t)))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 976))->q_line, "эта операция работает только с числами", v_op);
    }
    if (((v_to_ty == 1LL) && (v_t == 2LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 978))->q_line, "в int нельзя добавить дробь: сделай его dec", "");
    }
}

static void f_check__check_linkdecl(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 981))->q_kids, 0LL, sizeof(tb_o_Node *), 981));
    long long v_t = f_check__check_value(v_c, v_target);
    f_check__need_place_for(v_c, v_target, "ярлык link ведёт к переменной, u.age или nums[i]", 1);
    f_check__no_outside_change(v_c, v_target);
    if ((f_check__outside_temp(v_c, f_check__root_var(v_c, v_target)) && (!f_check__is_plain(v_t)))) {
        f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_n, 986))->q_line, ((tb_o_Node *)tb_nn(v_n, 986))->q_name);
    }
    { long long tb_v456 = v_t; long long *tb_p456 = &(((tb_o_Node *)tb_nn(v_n, 987))->q_ty); *tb_p456 = tb_v456; }
    f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 988))->q_name, v_t, 0, 1, ((tb_o_Node *)tb_nn(v_n, 988))->q_line);
}

static void f_check__check_multi(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 991))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 991))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 991));
    tb_o_Node * v_call = v_value;
    if ((({ const char *tb_s457 = ((tb_o_Node *)tb_nn(v_value, 993))->q_kind; ((unsigned char)tb_s457[0] == 116 && (unsigned char)tb_s457[1] == 114 && (unsigned char)tb_s457[2] == 121 && tb_s457[3] == 0); }) || ({ const char *tb_s458 = ((tb_o_Node *)tb_nn(v_value, 993))->q_kind; ((unsigned char)tb_s458[0] == 101 && (unsigned char)tb_s458[1] == 108 && (unsigned char)tb_s458[2] == 115 && (unsigned char)tb_s458[3] == 101 && (unsigned char)tb_s458[4] == 95 && (unsigned char)tb_s458[5] == 101 && (unsigned char)tb_s458[6] == 114 && (unsigned char)tb_s458[7] == 114 && tb_s458[8] == 0); }))) {
        v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 994))->q_kids, 0LL, sizeof(tb_o_Node *), 994));
    }
    if ((({ const char *tb_s459 = ((tb_o_Node *)tb_nn(v_call, 995))->q_kind; !((unsigned char)tb_s459[0] == 99 && (unsigned char)tb_s459[1] == 97 && (unsigned char)tb_s459[2] == 108 && (unsigned char)tb_s459[3] == 108 && tb_s459[4] == 0); }) && ({ const char *tb_s460 = ((tb_o_Node *)tb_nn(v_call, 995))->q_kind; !((unsigned char)tb_s460[0] == 109 && (unsigned char)tb_s460[1] == 101 && (unsigned char)tb_s460[2] == 116 && (unsigned char)tb_s460[3] == 104 && (unsigned char)tb_s460[4] == 111 && (unsigned char)tb_s460[5] == 100 && tb_s460[6] == 0); }))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 996))->q_line, "справа должен быть вызов функции с gives", "");
    }
    long long v_t = f_check__check_expr(v_c, v_value);
    long long v_names = ((((tb_o_Node *)tb_nn(v_n, 998))->q_kids)->len - 1LL);
    if (({ const char *tb_s461 = f_check__kind_of(v_c, v_t); !((unsigned char)tb_s461[0] == 109 && (unsigned char)tb_s461[1] == 117 && (unsigned char)tb_s461[2] == 108 && (unsigned char)tb_s461[3] == 116 && (unsigned char)tb_s461[4] == 105 && tb_s461[5] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1000))->q_line, "у этой функции один выход, пиши  x = ...", ((tb_o_Node *)tb_nn(v_call, 1000))->q_name);
    }
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1001))->q_types, v_t, sizeof(tb_o_Type *), 1001)), 1001))->q_func_node, 1001))->q_kids, 2LL, sizeof(tb_o_Node *), 1001));
    if (((((tb_o_Node *)tb_nn(v_outputs, 1002))->q_kids)->len != v_names)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1003))->q_line, "у функции другое число выходов", ((tb_o_Node *)tb_nn(v_call, 1003))->q_name);
    }
    {
        long long tb_end462 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end462; v_i++) {
            tb_o_Node * v_name = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1005))->q_kids, v_i, sizeof(tb_o_Node *), 1005));
            long long v_out = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1006))->q_kids, v_i, sizeof(tb_o_Node *), 1006)), 1006))->q_ty;
            long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_name, 1007))->q_name);
            if ((v_v < 0LL)) {
                { _Bool tb_v463 = 1; _Bool *tb_p463 = &(((tb_o_Node *)tb_nn(v_name, 1009))->q_creates); *tb_p463 = tb_v463; }
                f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_name, 1010))->q_name, v_out, 0, 0, ((tb_o_Node *)tb_nn(v_name, 1010))->q_line);
            } else {
                if (f_check__outside_par(v_c, v_v)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_name, 1013))->q_line, ({ const char *tb_c464_0 = "внутри "; long long tb_c464_0n = sizeof("внутри ") - 1; const char *tb_c464_1 = f_check__loop_word(v_c); long long tb_c464_1n = (long long)strlen(tb_c464_1); const char *tb_c464_2 = " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="; long long tb_c464_2n = sizeof(" нельзя менять переменную снаружи: сделай её shared и меняй через += или -=") - 1; char *tb_c464r = tb_mem(tb_c464_0n + tb_c464_1n + tb_c464_2n + 1), *tb_c464rw = tb_c464r; memcpy(tb_c464rw, tb_c464_0, tb_c464_0n); tb_c464rw += tb_c464_0n; memcpy(tb_c464rw, tb_c464_1, tb_c464_1n); tb_c464rw += tb_c464_1n; memcpy(tb_c464rw, tb_c464_2, tb_c464_2n); tb_c464rw += tb_c464_2n; *tb_c464rw = 0; (const char *)tb_c464r; }), ((tb_o_Node *)tb_nn(v_name, 1013))->q_name);
                }
                if ((f_check__outside_temp(v_c, v_v) && (!f_check__is_plain(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1014))->q_vars, v_v, sizeof(tb_o_Var *), 1014)), 1014))->q_type)))) {
                    f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_name, 1015))->q_line, ((tb_o_Node *)tb_nn(v_name, 1015))->q_name);
                }
                if (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1016))->q_vars, v_v, sizeof(tb_o_Var *), 1016)), 1016))->q_is_const) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_name, 1017))->q_line, "это const или переменная цикла, её нельзя менять", ((tb_o_Node *)tb_nn(v_name, 1017))->q_name);
                }
                if ((!f_check__fits(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1018))->q_vars, v_v, sizeof(tb_o_Var *), 1018)), 1018))->q_type, v_out))) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_name, 1019))->q_line, "переменная другого типа", f_check__type_name(v_c, ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1019))->q_vars, v_v, sizeof(tb_o_Var *), 1019)), 1019))->q_type));
                }
                { _Bool tb_v465 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1020))->q_vars, v_v, sizeof(tb_o_Var *), 1020)), 1020))->q_is_link; _Bool *tb_p465 = &(((tb_o_Node *)tb_nn(v_name, 1020))->q_is_link); *tb_p465 = tb_v465; }
            }
            { long long tb_v466 = v_out; long long *tb_p466 = &(((tb_o_Node *)tb_nn(v_name, 1021))->q_ty); *tb_p466 = tb_v466; }
        }
    }
}

static void f_check__check_return(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_f = ((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1024))->q_funcs, ((tb_o_Comp *)tb_nn(v_c, 1024))->q_current, sizeof(tb_o_Func *), 1024)), 1024))->q_node;
    if (((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1025))->q_kids, 2LL, sizeof(tb_o_Node *), 1025)), 1025))->q_kids)->len > 0LL)) {
        if (((((tb_o_Node *)tb_nn(v_n, 1026))->q_kids)->len != 0LL)) {
            if (((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1028))->q_kids, 2LL, sizeof(tb_o_Node *), 1028)), 1028))->q_kids)->len > 1LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1029))->q_line, "у функции с несколькими gives return пишется без значения: он отдаёт выходы", "");
            }
            long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1030))->q_kids, 0LL, sizeof(tb_o_Node *), 1030)));
            if ((!f_check__fits(((tb_o_Node *)tb_nn(v_f, 1031))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1032))->q_line, "return отдаёт другой тип, чем написано в gives", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_f, 1032))->q_ty));
            }
        }
        return;
    }
    long long v_t = 0LL;
    if (((((tb_o_Node *)tb_nn(v_n, 1035))->q_kids)->len == 1LL)) {
        v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1036))->q_kids, 0LL, sizeof(tb_o_Node *), 1036)));
    }
    if ((({ const char *tb_s467 = ((tb_o_Node *)tb_nn(v_f, 1037))->q_name; ((unsigned char)tb_s467[0] == 109 && (unsigned char)tb_s467[1] == 97 && (unsigned char)tb_s467[2] == 105 && (unsigned char)tb_s467[3] == 110 && tb_s467[4] == 0); }) && (v_t != 0LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1038))->q_line, "main ничего не возвращает: пиши просто return", "");
    }
    if ((((tb_o_Node *)tb_nn(v_f, 1039))->q_ty < 0LL)) {
        { long long tb_v468 = v_t; long long *tb_p468 = &(((tb_o_Node *)tb_nn(v_f, 1040))->q_ty); *tb_p468 = tb_v468; }
    } else if (((((tb_o_Node *)tb_nn(v_f, 1041))->q_ty == 1LL) && (v_t == 2LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1042))->q_line, "раньше функция вернула int, а тут dec: сделай оба числа одного типа", "");
    } else if ((!f_check__fits(((tb_o_Node *)tb_nn(v_f, 1043))->q_ty, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1044))->q_line, "раньше эта функция вернула другой тип", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_f, 1044))->q_ty));
    }
}

static void f_check__check_statement(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1047))->q_kind;
    if (({ const char *tb_s469 = v_k; ((unsigned char)tb_s469[0] == 97 && (unsigned char)tb_s469[1] == 115 && (unsigned char)tb_s469[2] == 115 && (unsigned char)tb_s469[3] == 105 && (unsigned char)tb_s469[4] == 103 && (unsigned char)tb_s469[5] == 110 && tb_s469[6] == 0); })) {
        f_check__check_assign(v_c, v_n);
    } else if (({ const char *tb_s470 = v_k; ((unsigned char)tb_s470[0] == 115 && (unsigned char)tb_s470[1] == 101 && (unsigned char)tb_s470[2] == 116 && tb_s470[3] == 0); })) {
        f_check__check_set(v_c, v_n);
    } else if (({ const char *tb_s471 = v_k; ((unsigned char)tb_s471[0] == 108 && (unsigned char)tb_s471[1] == 105 && (unsigned char)tb_s471[2] == 110 && (unsigned char)tb_s471[3] == 107 && (unsigned char)tb_s471[4] == 100 && (unsigned char)tb_s471[5] == 101 && (unsigned char)tb_s471[6] == 99 && (unsigned char)tb_s471[7] == 108 && tb_s471[8] == 0); })) {
        f_check__check_linkdecl(v_c, v_n);
    } else if (({ const char *tb_s472 = v_k; ((unsigned char)tb_s472[0] == 109 && (unsigned char)tb_s472[1] == 117 && (unsigned char)tb_s472[2] == 108 && (unsigned char)tb_s472[3] == 116 && (unsigned char)tb_s472[4] == 105 && tb_s472[5] == 0); })) {
        f_check__check_multi(v_c, v_n);
    } else if (({ const char *tb_s473 = v_k; ((unsigned char)tb_s473[0] == 105 && (unsigned char)tb_s473[1] == 102 && tb_s473[2] == 0); })) {
        long long v_i = 0LL;
        while (((v_i + 1LL) < (((tb_o_Node *)tb_nn(v_n, 1058))->q_kids)->len)) {
            f_check__need_bool(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1059))->q_kids, v_i, sizeof(tb_o_Node *), 1059)));
            f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1060))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 1060)));
            { long long tb_v474 = 2LL; long long *tb_p474 = &(v_i); *tb_p474 += tb_v474; }
        }
        if ((tb_mod((((tb_o_Node *)tb_nn(v_n, 1062))->q_kids)->len, 2LL, 1062) == 1LL)) {
            f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1063))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 1063))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 1063)));
        }
    } else if (({ const char *tb_s475 = v_k; ((unsigned char)tb_s475[0] == 101 && (unsigned char)tb_s475[1] == 97 && (unsigned char)tb_s475[2] == 99 && (unsigned char)tb_s475[3] == 104 && tb_s475[4] == 0); })) {
        long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1065))->q_kids, 0LL, sizeof(tb_o_Node *), 1065)));
        long long v_elem = 4LL;
        if (f_check__is_list(v_c, v_t)) {
            v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1068))->q_types, v_t, sizeof(tb_o_Type *), 1068)), 1068))->q_elem;
        } else if (f_check__is_dict(v_c, v_t)) {
            v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1070))->q_types, v_t, sizeof(tb_o_Type *), 1070)), 1070))->q_key;
        } else if ((v_t != 4LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1072))->q_line, "each ходит по списку, по ключам dict или по буквам текста", "");
        }
        if ((((tb_o_Node *)tb_nn(v_n, 1073))->q_is_par && (!f_check__is_list(v_c, v_t)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1074))->q_line, "on all cores ходит по списку (или count from ... to ...)", "");
        }
        if ((((tb_o_Node *)tb_nn(v_n, 1075))->q_by_vec && (!(f_check__is_list(v_c, v_t) && f_check__is_number(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1075))->q_types, v_t, sizeof(tb_o_Type *), 1075)), 1075))->q_elem))))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1076))->q_line, "by vec ходит по списку чисел: list int или list dec", "");
        }
        if (((tb_o_Node *)tb_nn(v_n, 1077))->q_by_piece) {
            f_check__check_pieces(v_c, v_n, v_t);
        } else {
            f_check__open_scope(v_c);
            f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1081))->q_name, v_elem, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1081))->q_line);
            { tb_o_Node * tb_v476 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1082))->q_loops) = tb_v476; }
            f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1083))->q_kids, 1LL, sizeof(tb_o_Node *), 1083)));
            tb_o_Node * v_gone = (*(tb_o_Node * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 1084))->q_loops, 1084));
            f_check__close_scope(v_c);
        }
    } else if (({ const char *tb_s477 = v_k; ((unsigned char)tb_s477[0] == 99 && (unsigned char)tb_s477[1] == 111 && (unsigned char)tb_s477[2] == 117 && (unsigned char)tb_s477[3] == 110 && (unsigned char)tb_s477[4] == 116 && tb_s477[5] == 0); })) {
        f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1087))->q_kids, 0LL, sizeof(tb_o_Node *), 1087)), "from");
        f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1088))->q_kids, 1LL, sizeof(tb_o_Node *), 1088)), "to");
        f_check__open_scope(v_c);
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1090))->q_name, 1LL, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1090))->q_line);
        { tb_o_Node * tb_v478 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1091))->q_loops) = tb_v478; }
        f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1092))->q_kids, 2LL, sizeof(tb_o_Node *), 1092)));
        tb_o_Node * v_gone = (*(tb_o_Node * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 1093))->q_loops, 1093));
        f_check__close_scope(v_c);
    } else if (((strcmp(v_k, "repeat_times") == 0) || (strcmp(v_k, "repeat_while") == 0))) {
        if ((strcmp(v_k, "repeat_times") == 0)) {
            f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1097))->q_kids, 0LL, sizeof(tb_o_Node *), 1097)), "times");
        } else {
            f_check__need_bool(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1099))->q_kids, 0LL, sizeof(tb_o_Node *), 1099)));
        }
        { long long tb_v479 = 1LL; long long *tb_p479 = &(((tb_o_Comp *)tb_nn(v_c, 1100))->q_loop_depth); *tb_p479 += tb_v479; }
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1101))->q_kids, 1LL, sizeof(tb_o_Node *), 1101)));
        { long long tb_v480 = 1LL; long long *tb_p480 = &(((tb_o_Comp *)tb_nn(v_c, 1102))->q_loop_depth); *tb_p480 -= tb_v480; }
    } else if (({ const char *tb_s481 = v_k; ((unsigned char)tb_s481[0] == 116 && (unsigned char)tb_s481[1] == 111 && (unsigned char)tb_s481[2] == 103 && (unsigned char)tb_s481[3] == 101 && (unsigned char)tb_s481[4] == 116 && (unsigned char)tb_s481[5] == 104 && (unsigned char)tb_s481[6] == 101 && (unsigned char)tb_s481[7] == 114 && tb_s481[8] == 0); })) {
        f_check__check_together(v_c, v_n);
    } else if (({ const char *tb_s482 = v_k; ((unsigned char)tb_s482[0] == 116 && (unsigned char)tb_s482[1] == 101 && (unsigned char)tb_s482[2] == 109 && (unsigned char)tb_s482[3] == 112 && tb_s482[4] == 0); })) {
        long long v_saved_base = ((tb_o_Comp *)tb_nn(v_c, 1106))->q_temp_base;
        long long v_saved_loops = ((tb_o_Comp *)tb_nn(v_c, 1107))->q_temp_loops;
        { long long tb_v483 = (((tb_o_Comp *)tb_nn(v_c, 1108))->q_vars)->len; long long *tb_p483 = &(((tb_o_Comp *)tb_nn(v_c, 1108))->q_temp_base); *tb_p483 = tb_v483; }
        { long long tb_v484 = ((tb_o_Comp *)tb_nn(v_c, 1109))->q_loop_depth; long long *tb_p484 = &(((tb_o_Comp *)tb_nn(v_c, 1109))->q_temp_loops); *tb_p484 = tb_v484; }
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1110))->q_kids, 0LL, sizeof(tb_o_Node *), 1110)));
        { long long tb_v485 = v_saved_base; long long *tb_p485 = &(((tb_o_Comp *)tb_nn(v_c, 1111))->q_temp_base); *tb_p485 = tb_v485; }
        { long long tb_v486 = v_saved_loops; long long *tb_p486 = &(((tb_o_Comp *)tb_nn(v_c, 1112))->q_temp_loops); *tb_p486 = tb_v486; }
    } else if ((({ const char *tb_s487 = v_k; ((unsigned char)tb_s487[0] == 115 && (unsigned char)tb_s487[1] == 107 && (unsigned char)tb_s487[2] == 105 && (unsigned char)tb_s487[3] == 112 && tb_s487[4] == 0); }) || ({ const char *tb_s488 = v_k; ((unsigned char)tb_s488[0] == 115 && (unsigned char)tb_s488[1] == 116 && (unsigned char)tb_s488[2] == 111 && (unsigned char)tb_s488[3] == 112 && tb_s488[4] == 0); }))) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1114))->q_loop_depth == 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1115))->q_line, "skip и stop пишутся только внутри цикла", "");
        }
        if (((({ const char *tb_s489 = v_k; ((unsigned char)tb_s489[0] == 115 && (unsigned char)tb_s489[1] == 116 && (unsigned char)tb_s489[2] == 111 && (unsigned char)tb_s489[3] == 112 && tb_s489[4] == 0); }) && (((tb_o_Comp *)tb_nn(v_c, 1116))->q_par_base >= 0LL)) && (((tb_o_Comp *)tb_nn(v_c, 1116))->q_loop_depth == ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1116))->q_par_node, 1116))->q_temp_no))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1117))->q_line, "stop внутри on all cores нельзя: остальные ядра всё равно работают", "");
        }
        if (((((tb_o_Comp *)tb_nn(v_c, 1118))->q_temp_base >= 0LL) && (((tb_o_Comp *)tb_nn(v_c, 1118))->q_loop_depth == ((tb_o_Comp *)tb_nn(v_c, 1118))->q_temp_loops))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1119))->q_line, "skip и stop внутри temp работают только для циклов внутри temp", "");
        }
    } else if (({ const char *tb_s490 = v_k; ((unsigned char)tb_s490[0] == 114 && (unsigned char)tb_s490[1] == 101 && (unsigned char)tb_s490[2] == 116 && (unsigned char)tb_s490[3] == 117 && (unsigned char)tb_s490[4] == 114 && (unsigned char)tb_s490[5] == 110 && tb_s490[6] == 0); })) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1121))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1122))->q_line, ({ const char *tb_c491_0 = "внутри "; long long tb_c491_0n = sizeof("внутри ") - 1; const char *tb_c491_1 = f_check__loop_word(v_c); long long tb_c491_1n = (long long)strlen(tb_c491_1); const char *tb_c491_2 = " return нельзя"; long long tb_c491_2n = sizeof(" return нельзя") - 1; char *tb_c491r = tb_mem(tb_c491_0n + tb_c491_1n + tb_c491_2n + 1), *tb_c491rw = tb_c491r; memcpy(tb_c491rw, tb_c491_0, tb_c491_0n); tb_c491rw += tb_c491_0n; memcpy(tb_c491rw, tb_c491_1, tb_c491_1n); tb_c491rw += tb_c491_1n; memcpy(tb_c491rw, tb_c491_2, tb_c491_2n); tb_c491rw += tb_c491_2n; *tb_c491rw = 0; (const char *)tb_c491r; }), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1123))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1124))->q_line, "внутри temp return нельзя: сохрани число в переменную снаружи и выйди из блока", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1125))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1126))->q_line, "внутри each ... by N return нельзя: сохрани число в переменную снаружи и выйди через stop", "");
        }
        f_check__check_return(v_c, v_n);
    } else if (({ const char *tb_s492 = v_k; ((unsigned char)tb_s492[0] == 102 && (unsigned char)tb_s492[1] == 97 && (unsigned char)tb_s492[2] == 105 && (unsigned char)tb_s492[3] == 108 && tb_s492[4] == 0); })) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1129))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1130))->q_line, ({ const char *tb_c493_0 = "внутри "; long long tb_c493_0n = sizeof("внутри ") - 1; const char *tb_c493_1 = f_check__loop_word(v_c); long long tb_c493_1n = (long long)strlen(tb_c493_1); const char *tb_c493_2 = " fail нельзя: пиши  or  или  else err"; long long tb_c493_2n = sizeof(" fail нельзя: пиши  or  или  else err") - 1; char *tb_c493r = tb_mem(tb_c493_0n + tb_c493_1n + tb_c493_2n + 1), *tb_c493rw = tb_c493r; memcpy(tb_c493rw, tb_c493_0, tb_c493_0n); tb_c493rw += tb_c493_0n; memcpy(tb_c493rw, tb_c493_1, tb_c493_1n); tb_c493rw += tb_c493_1n; memcpy(tb_c493rw, tb_c493_2, tb_c493_2n); tb_c493rw += tb_c493_2n; *tb_c493rw = 0; (const char *)tb_c493r; }), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1131))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1132))->q_line, "внутри temp fail нельзя: пиши  or  или  else err", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1133))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1134))->q_line, "внутри each ... by N fail нельзя: пиши  or  или  else err", "");
        }
        if ((f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1135))->q_kids, 0LL, sizeof(tb_o_Node *), 1135))) != 4LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1136))->q_line, "после fail пишется текст ошибки: fail \"нет файла\"", "");
        }
    } else if ((strcmp(v_k, "expr_stmt") == 0)) {
        tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1138))->q_kids, 0LL, sizeof(tb_o_Node *), 1138));
        tb_o_Node * v_inner = v_e;
        if ((({ const char *tb_s494 = ((tb_o_Node *)tb_nn(v_e, 1140))->q_kind; ((unsigned char)tb_s494[0] == 116 && (unsigned char)tb_s494[1] == 114 && (unsigned char)tb_s494[2] == 121 && tb_s494[3] == 0); }) || ({ const char *tb_s495 = ((tb_o_Node *)tb_nn(v_e, 1140))->q_kind; ((unsigned char)tb_s495[0] == 101 && (unsigned char)tb_s495[1] == 108 && (unsigned char)tb_s495[2] == 115 && (unsigned char)tb_s495[3] == 101 && (unsigned char)tb_s495[4] == 95 && (unsigned char)tb_s495[5] == 101 && (unsigned char)tb_s495[6] == 114 && (unsigned char)tb_s495[7] == 114 && tb_s495[8] == 0); }))) {
            v_inner = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1141))->q_kids, 0LL, sizeof(tb_o_Node *), 1141));
        }
        if (((({ const char *tb_s496 = ((tb_o_Node *)tb_nn(v_inner, 1142))->q_kind; !((unsigned char)tb_s496[0] == 99 && (unsigned char)tb_s496[1] == 97 && (unsigned char)tb_s496[2] == 108 && (unsigned char)tb_s496[3] == 108 && tb_s496[4] == 0); }) && ({ const char *tb_s497 = ((tb_o_Node *)tb_nn(v_inner, 1142))->q_kind; !((unsigned char)tb_s497[0] == 109 && (unsigned char)tb_s497[1] == 101 && (unsigned char)tb_s497[2] == 116 && (unsigned char)tb_s497[3] == 104 && (unsigned char)tb_s497[4] == 111 && (unsigned char)tb_s497[5] == 100 && tb_s497[6] == 0); })) && (!(({ const char *tb_s498 = ((tb_o_Node *)tb_nn(v_inner, 1142))->q_kind; ((unsigned char)tb_s498[0] == 102 && (unsigned char)tb_s498[1] == 105 && (unsigned char)tb_s498[2] == 101 && (unsigned char)tb_s498[3] == 108 && (unsigned char)tb_s498[4] == 100 && tb_s498[5] == 0); }) && ({ const char *tb_s499 = ((tb_o_Node *)tb_nn(v_inner, 1142))->q_name; ((unsigned char)tb_s499[0] == 119 && (unsigned char)tb_s499[1] == 97 && (unsigned char)tb_s499[2] == 105 && (unsigned char)tb_s499[3] == 116 && tb_s499[4] == 0); }))))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1143))->q_line, "строка ничего не делает", "");
        }
        long long v_t = f_check__check_expr(v_c, v_e);
        if ((v_t != 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1146))->q_line, "результат вызова пропадает: сохрани его, например x = ...", ((tb_o_Node *)tb_nn(v_e, 1146))->q_name);
        }
    }
}

static long long f_check__check_background(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    if ((((tb_o_Comp *)tb_nn(v_c, 1152))->q_par_base >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1153))->q_line, ({ const char *tb_c500_0 = "внутри "; long long tb_c500_0n = sizeof("внутри ") - 1; const char *tb_c500_1 = f_check__loop_word(v_c); long long tb_c500_1n = (long long)strlen(tb_c500_1); const char *tb_c500_2 = " background нельзя: цикл и так идёт на всех ядрах"; long long tb_c500_2n = sizeof(" background нельзя: цикл и так идёт на всех ядрах") - 1; char *tb_c500r = tb_mem(tb_c500_0n + tb_c500_1n + tb_c500_2n + 1), *tb_c500rw = tb_c500r; memcpy(tb_c500rw, tb_c500_0, tb_c500_0n); tb_c500rw += tb_c500_0n; memcpy(tb_c500rw, tb_c500_1, tb_c500_1n); tb_c500rw += tb_c500_1n; memcpy(tb_c500rw, tb_c500_2, tb_c500_2n); tb_c500rw += tb_c500_2n; *tb_c500rw = 0; (const char *)tb_c500r; }), "");
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1154))->q_temp_base >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1155))->q_line, "внутри temp background нельзя: память блока удалится, пока функция ещё работает", "");
    }
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1156))->q_kids, 0LL, sizeof(tb_o_Node *), 1156));
    if ((({ const char *tb_s501 = ((tb_o_Node *)tb_nn(v_call, 1157))->q_kind; ((unsigned char)tb_s501[0] == 99 && (unsigned char)tb_s501[1] == 97 && (unsigned char)tb_s501[2] == 108 && (unsigned char)tb_s501[3] == 108 && tb_s501[4] == 0); }) && f_check__is_builtin(((tb_o_Node *)tb_nn(v_call, 1157))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1158))->q_line, "background запускает свою функцию: a = background load(name)", ((tb_o_Node *)tb_nn(v_call, 1158))->q_name);
    }
    { _Bool tb_v502 = 1; _Bool *tb_p502 = &(((tb_o_Node *)tb_nn(v_call, 1159))->q_creates); *tb_p502 = tb_v502; }
    long long v_t = f_check__check_expr(v_c, v_call);
    if (((({ const char *tb_s503 = ((tb_o_Node *)tb_nn(v_call, 1161))->q_kind; !((unsigned char)tb_s503[0] == 99 && (unsigned char)tb_s503[1] == 97 && (unsigned char)tb_s503[2] == 108 && (unsigned char)tb_s503[3] == 108 && tb_s503[4] == 0); }) || (!((tb_o_Node *)tb_nn(v_call, 1161))->q_has_ref)) || ({ const char *tb_s504 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_call, 1161))->q_ref, 1161))->q_kind; !((unsigned char)tb_s504[0] == 102 && (unsigned char)tb_s504[1] == 117 && (unsigned char)tb_s504[2] == 110 && (unsigned char)tb_s504[3] == 99 && tb_s504[4] == 0); }))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1162))->q_line, "background запускает свою функцию: a = background load(name)", "");
    }
    tb_o_Node * v_f = ((tb_o_Node *)tb_nn(v_call, 1163))->q_ref;
    if (((tb_o_Node *)tb_nn(v_f, 1164))->q_can_fail) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1165))->q_line, "функция с fail в background пока нельзя: пусть она отдаёт текст ошибки обычным выходом", ((tb_o_Node *)tb_nn(v_f, 1165))->q_name);
    }
    if (((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1166))->q_kids, 2LL, sizeof(tb_o_Node *), 1166)), 1166))->q_kids)->len > 1LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1167))->q_line, "в background функция с несколькими выходами пока нельзя", ((tb_o_Node *)tb_nn(v_f, 1167))->q_name);
    }
    {
        tb_list * tb_l505 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1168))->q_kids, 0LL, sizeof(tb_o_Node *), 1168)), 1168))->q_kids;
        for (long long tb_i505 = 0; tb_i505 < tb_l505->len; tb_i505++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l505->data)[tb_i505];
            if ((((tb_o_Node *)tb_nn(v_p, 1169))->q_is_link || (!(f_check__is_plain(((tb_o_Node *)tb_nn(v_p, 1169))->q_ty) || (((tb_o_Node *)tb_nn(v_p, 1169))->q_ty == 4LL))))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1170))->q_line, "в background функция получает только int, dec, bool и text: списки и obj могли бы меняться сразу из двух мест", ((tb_o_Node *)tb_nn(v_p, 1170))->q_name);
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
    tb_o_Node * v_block = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1178))->q_kids, 0LL, sizeof(tb_o_Node *), 1178));
    tb_list * v_names = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l506 = ((tb_o_Node *)tb_nn(v_block, 1180))->q_kids;
        for (long long tb_i506 = 0; tb_i506 < tb_l506->len; tb_i506++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l506->data)[tb_i506];
            if ((({ const char *tb_s507 = ((tb_o_Node *)tb_nn(v_s, 1181))->q_kind; !((unsigned char)tb_s507[0] == 97 && (unsigned char)tb_s507[1] == 115 && (unsigned char)tb_s507[2] == 115 && (unsigned char)tb_s507[3] == 105 && (unsigned char)tb_s507[4] == 103 && (unsigned char)tb_s507[5] == 110 && tb_s507[6] == 0); }) || ((((tb_o_Node *)tb_nn(v_s, 1181))->q_kids)->len != 1LL))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1182))->q_line, "в together пишутся только новые переменные со значением: a = d[key]", "");
            }
            if ((f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_s, 1183))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1184))->q_line, "в together заводятся только новые переменные, а эта уже есть", ((tb_o_Node *)tb_nn(v_s, 1184))->q_name);
            }
            {
                tb_list * tb_l508 = v_names;
                for (long long tb_i508 = 0; tb_i508 < tb_l508->len; tb_i508++) {
                    const char * const v_w = ((const char * *)tb_l508->data)[tb_i508];
                    if ((strcmp(v_w, ((tb_o_Node *)tb_nn(v_s, 1186))->q_name) == 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1187))->q_line, "в together эта переменная уже заведена строкой выше", ((tb_o_Node *)tb_nn(v_s, 1187))->q_name);
                    }
                    if (f_check__uses_name((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_s, 1188))->q_kids, 0LL, sizeof(tb_o_Node *), 1188)), v_w)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1189))->q_line, "строки в together не должны зависеть друг от друга, а здесь нужна", v_w);
                    }
                }
            }
            { const char * tb_v509 = ((tb_o_Node *)tb_nn(v_s, 1190))->q_name; *(const char * *)tb_grow(v_names) = tb_v509; }
            f_check__check_statement(v_c, v_s);
        }
    }
}

static _Bool f_check__uses_name(tb_o_Node * v_n, const char * v_name) {
    if ((({ const char *tb_s510 = ((tb_o_Node *)tb_nn(v_n, 1195))->q_kind; ((unsigned char)tb_s510[0] == 110 && (unsigned char)tb_s510[1] == 97 && (unsigned char)tb_s510[2] == 109 && (unsigned char)tb_s510[3] == 101 && tb_s510[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1195))->q_name, v_name) == 0))) {
        return 1;
    }
    {
        tb_list * tb_l511 = ((tb_o_Node *)tb_nn(v_n, 1197))->q_kids;
        for (long long tb_i511 = 0; tb_i511 < tb_l511->len; tb_i511++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l511->data)[tb_i511];
            if (f_check__uses_name(v_k, v_name)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1194, "функция uses_name дошла до конца без return");
    return 0;
}

static void f_check__check_pieces(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t) {
    if ((!f_check__is_list(v_c, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1206))->q_line, "by N режет на куски список: each piece in nums by 64", "");
    }
    if (f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1207))->q_types, v_t, sizeof(tb_o_Type *), 1207)), 1207))->q_elem)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1208))->q_line, "список колонок (by columns) на куски пока не режется", "");
    }
    if ((((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1209))->q_types, v_t, sizeof(tb_o_Type *), 1209)), 1209))->q_elem == 3LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1210))->q_line, "list bool хранится битами и на куски пока не режется", "");
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1211))->q_par_base >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1212))->q_line, ({ const char *tb_c512_0 = "each ... by N внутри "; long long tb_c512_0n = sizeof("each ... by N внутри ") - 1; const char *tb_c512_1 = f_check__loop_word(v_c); long long tb_c512_1n = (long long)strlen(tb_c512_1); const char *tb_c512_2 = " пока нельзя"; long long tb_c512_2n = sizeof(" пока нельзя") - 1; char *tb_c512r = tb_mem(tb_c512_0n + tb_c512_1n + tb_c512_2n + 1), *tb_c512rw = tb_c512r; memcpy(tb_c512rw, tb_c512_0, tb_c512_0n); tb_c512rw += tb_c512_0n; memcpy(tb_c512rw, tb_c512_1, tb_c512_1n); tb_c512rw += tb_c512_1n; memcpy(tb_c512rw, tb_c512_2, tb_c512_2n); tb_c512rw += tb_c512_2n; *tb_c512rw = 0; (const char *)tb_c512r; }), "");
    }
    f_check__need_int(v_c, ((tb_o_Node *)tb_nn(v_n, 1213))->q_piece_size, "by");
    { const char * tb_v513 = ((tb_o_Comp *)tb_nn(v_c, 1214))->q_file_name; const char * *tb_p513 = &(((tb_o_Node *)tb_nn(v_n, 1214))->q_file); *tb_p513 = tb_v513; }
    { tb_o_Node * tb_v514 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1215))->q_pieces) = tb_v514; }
    f_check__open_scope(v_c);
    f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1217))->q_name, v_t, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1217))->q_line);
    { long long tb_v515 = 1LL; long long *tb_p515 = &(((tb_o_Comp *)tb_nn(v_c, 1218))->q_piece_depth); *tb_p515 += tb_v515; }
    { tb_o_Node * tb_v516 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1219))->q_loops) = tb_v516; }
    f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1220))->q_kids, 1LL, sizeof(tb_o_Node *), 1220)));
    tb_o_Node * v_gone = (*(tb_o_Node * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 1221))->q_loops, 1221));
    { long long tb_v517 = 1LL; long long *tb_p517 = &(((tb_o_Comp *)tb_nn(v_c, 1222))->q_piece_depth); *tb_p517 -= tb_v517; }
    f_check__close_scope(v_c);
}

static _Bool f_check__is_name_of(tb_o_Node * v_v, const char * v_name) {
    return (({ const char *tb_s518 = ((tb_o_Node *)tb_nn(v_v, 1227))->q_kind; ((unsigned char)tb_s518[0] == 110 && (unsigned char)tb_s518[1] == 97 && (unsigned char)tb_s518[2] == 109 && (unsigned char)tb_s518[3] == 101 && tb_s518[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_v, 1227))->q_name, v_name) == 0));
    tb_crash(1226, "функция is_name_of дошла до конца без return");
    return 0;
}

static long long f_check__piece_leak(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_name) {
    long long v_r = 0LL;
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1233))->q_kind;
    if ((({ const char *tb_s519 = v_k; ((unsigned char)tb_s519[0] == 110 && (unsigned char)tb_s519[1] == 97 && (unsigned char)tb_s519[2] == 109 && (unsigned char)tb_s519[3] == 101 && tb_s519[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1234))->q_name, v_name) == 0))) {
        return ((tb_o_Node *)tb_nn(v_n, 1235))->q_line;
    }
    if (((({ const char *tb_s520 = v_k; ((unsigned char)tb_s520[0] == 97 && (unsigned char)tb_s520[1] == 115 && (unsigned char)tb_s520[2] == 115 && (unsigned char)tb_s520[3] == 105 && (unsigned char)tb_s520[4] == 103 && (unsigned char)tb_s520[5] == 110 && tb_s520[6] == 0); }) || ({ const char *tb_s521 = v_k; ((unsigned char)tb_s521[0] == 108 && (unsigned char)tb_s521[1] == 105 && (unsigned char)tb_s521[2] == 110 && (unsigned char)tb_s521[3] == 107 && (unsigned char)tb_s521[4] == 100 && (unsigned char)tb_s521[5] == 101 && (unsigned char)tb_s521[6] == 99 && (unsigned char)tb_s521[7] == 108 && tb_s521[8] == 0); })) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1236))->q_name, v_name) == 0))) {
        return ((tb_o_Node *)tb_nn(v_n, 1237))->q_line;
    }
    if (((({ const char *tb_s522 = v_k; ((unsigned char)tb_s522[0] == 99 && (unsigned char)tb_s522[1] == 97 && (unsigned char)tb_s522[2] == 108 && (unsigned char)tb_s522[3] == 108 && tb_s522[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1238))->q_has_ref) && ({ const char *tb_s523 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1238))->q_ref, 1238))->q_kind; ((unsigned char)tb_s523[0] == 102 && (unsigned char)tb_s523[1] == 117 && (unsigned char)tb_s523[2] == 110 && (unsigned char)tb_s523[3] == 99 && tb_s523[4] == 0); }))) {
        {
            long long tb_end524 = ((((tb_o_Node *)tb_nn(v_n, 1239))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end524; v_i++) {
                tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1240))->q_kids, v_i, sizeof(tb_o_Node *), 1240)), 1240))->q_kids, 0LL, sizeof(tb_o_Node *), 1240));
                if (f_check__is_name_of(v_v, v_name)) {
                    if ((!f_check__param_keeps_piece(v_c, ((tb_o_Node *)tb_nn(v_n, 1242))->q_ref, v_i))) {
                        continue;
                    }
                    return ((tb_o_Node *)tb_nn(v_v, 1244))->q_line;
                }
                long long v_bad = f_check__piece_leak(v_c, v_v, v_name);
                if ((v_bad != 0LL)) {
                    return v_bad;
                }
            }
        }
        return 0LL;
    }
    if ((({ const char *tb_s525 = v_k; ((unsigned char)tb_s525[0] == 99 && (unsigned char)tb_s525[1] == 97 && (unsigned char)tb_s525[2] == 108 && (unsigned char)tb_s525[3] == 108 && tb_s525[4] == 0); }) && f_check__is_builtin(((tb_o_Node *)tb_nn(v_n, 1249))->q_name))) {
        {
            tb_list * tb_l526 = ((tb_o_Node *)tb_nn(v_n, 1250))->q_kids;
            for (long long tb_i526 = 0; tb_i526 < tb_l526->len; tb_i526++) {
                tb_o_Node * const v_a = ((tb_o_Node * *)tb_l526->data)[tb_i526];
                tb_o_Node * v_v = v_a;
                if (({ const char *tb_s527 = ((tb_o_Node *)tb_nn(v_a, 1252))->q_kind; ((unsigned char)tb_s527[0] == 97 && (unsigned char)tb_s527[1] == 114 && (unsigned char)tb_s527[2] == 103 && tb_s527[3] == 0); })) {
                    v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_a, 1253))->q_kids, 0LL, sizeof(tb_o_Node *), 1253));
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
    if ((((({ const char *tb_s528 = v_k; ((unsigned char)tb_s528[0] == 105 && (unsigned char)tb_s528[1] == 110 && (unsigned char)tb_s528[2] == 100 && (unsigned char)tb_s528[3] == 101 && (unsigned char)tb_s528[4] == 120 && tb_s528[5] == 0); }) || ({ const char *tb_s529 = v_k; ((unsigned char)tb_s529[0] == 101 && (unsigned char)tb_s529[1] == 97 && (unsigned char)tb_s529[2] == 99 && (unsigned char)tb_s529[3] == 104 && tb_s529[4] == 0); })) && ((((tb_o_Node *)tb_nn(v_n, 1260))->q_kids)->len > 0LL)) && f_check__is_name_of((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1260))->q_kids, 0LL, sizeof(tb_o_Node *), 1260)), v_name))) {
        v_start = 1LL;
    }
    if ((({ const char *tb_s530 = v_k; ((unsigned char)tb_s530[0] == 101 && (unsigned char)tb_s530[1] == 97 && (unsigned char)tb_s530[2] == 99 && (unsigned char)tb_s530[3] == 104 && tb_s530[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1262))->q_by_piece)) {
        long long v_bad = f_check__piece_leak(v_c, ((tb_o_Node *)tb_nn(v_n, 1263))->q_piece_size, v_name);
        if ((v_bad != 0LL)) {
            return v_bad;
        }
    }
    {
        long long tb_end531 = ((((tb_o_Node *)tb_nn(v_n, 1266))->q_kids)->len - 1LL);
        for (long long v_i = v_start; v_i <= tb_end531; v_i++) {
            long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1267))->q_kids, v_i, sizeof(tb_o_Node *), 1267)), v_name);
            if ((v_bad != 0LL)) {
                return v_bad;
            }
        }
    }
    return 0LL;
    return v_r;
}

static _Bool f_check__param_keeps_piece(tb_o_Comp * v_c, tb_o_Node * v_f, long long v_i) {
    const char * v_key = ({ const char *tb_c532_0 = ((tb_o_Node *)tb_nn(v_f, 1274))->q_name; long long tb_c532_0n = (long long)strlen(tb_c532_0); const char *tb_c532_1 = "@"; long long tb_c532_1n = sizeof("@") - 1; const char *tb_c532_2 = ((tb_o_Node *)tb_nn(v_f, 1274))->q_mod; long long tb_c532_2n = (long long)strlen(tb_c532_2); const char *tb_c532_3 = "#"; long long tb_c532_3n = sizeof("#") - 1; char tb_c532_4d[24]; long long tb_c532_4n; const char *tb_c532_4 = tb_digits(v_i, tb_c532_4d, &tb_c532_4n); char *tb_c532r = tb_mem(tb_c532_0n + tb_c532_1n + tb_c532_2n + tb_c532_3n + tb_c532_4n + 1), *tb_c532rw = tb_c532r; memcpy(tb_c532rw, tb_c532_0, tb_c532_0n); tb_c532rw += tb_c532_0n; memcpy(tb_c532rw, tb_c532_1, tb_c532_1n); tb_c532rw += tb_c532_1n; memcpy(tb_c532rw, tb_c532_2, tb_c532_2n); tb_c532rw += tb_c532_2n; memcpy(tb_c532rw, tb_c532_3, tb_c532_3n); tb_c532rw += tb_c532_3n; memcpy(tb_c532rw, tb_c532_4, tb_c532_4n); tb_c532rw += tb_c532_4n; *tb_c532rw = 0; (const char *)tb_c532r; });
    {
        long long tb_end533 = ((((tb_o_Comp *)tb_nn(v_c, 1275))->q_safe_keys)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end533; v_j++) {
            if ((strcmp((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1276))->q_safe_keys, v_j, sizeof(const char *), 1276)), v_key) == 0)) {
                return ((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1277))->q_safe_vals, v_j, sizeof(long long), 1277)) == 2LL);
            }
        }
    }
    { const char * tb_v534 = v_key; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1279))->q_safe_keys) = tb_v534; }
    { long long tb_v535 = 1LL; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1280))->q_safe_vals) = tb_v535; }
    long long v_slot = ((((tb_o_Comp *)tb_nn(v_c, 1281))->q_safe_keys)->len - 1LL);
    { long long tb_v536 = 1LL; long long *tb_p536 = &(((tb_o_Comp *)tb_nn(v_c, 1282))->q_safe_busy); *tb_p536 += tb_v536; }
    long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1283))->q_kids, 1LL, sizeof(tb_o_Node *), 1283)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1283))->q_kids, 0LL, sizeof(tb_o_Node *), 1283)), 1283))->q_kids, v_i, sizeof(tb_o_Node *), 1283)), 1283))->q_name);
    { long long tb_v537 = 1LL; long long *tb_p537 = &(((tb_o_Comp *)tb_nn(v_c, 1284))->q_safe_busy); *tb_p537 -= tb_v537; }
    if ((v_bad != 0LL)) {
        { long long tb_v538 = 2LL; long long *tb_p538 = &((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1286))->q_safe_vals, v_slot, sizeof(long long), 1286))); *tb_p538 = tb_v538; }
        return 1;
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1288))->q_safe_busy > 0LL)) {
        { long long tb_v539 = 3LL; long long *tb_p539 = &((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1290))->q_safe_vals, v_slot, sizeof(long long), 1290))); *tb_p539 = tb_v539; }
        { const char * tb_v540 = ""; const char * *tb_p540 = &((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1291))->q_safe_keys, v_slot, sizeof(const char *), 1291))); *tb_p540 = tb_v540; }
    }
    return 0;
    tb_crash(1273, "функция param_keeps_piece дошла до конца без return");
    return 0;
}

static void f_check__mark_hot_fields(tb_o_Comp * v_c) {
    tb_list * v_hot_func = tb_bits_new();
    {
        long long tb_end541 = (((tb_o_Comp *)tb_nn(v_c, 1301))->q_funcs)->len;
        for (long long v_i = 1LL; v_i <= tb_end541; v_i++) {
            { _Bool tb_v542 = 0; tb_badd(v_hot_func, tb_v542); }
        }
    }
    {
        long long tb_end543 = ((((tb_o_Comp *)tb_nn(v_c, 1303))->q_call_to)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end543; v_j++) {
            if (tb_bget(((tb_o_Comp *)tb_nn(v_c, 1304))->q_call_loop, v_j, 1304)) {
                { _Bool tb_v544 = 1; tb_bset(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1305))->q_call_to, v_j, sizeof(long long), 1305)), tb_v544, 1305); }
            }
        }
    }
    _Bool v_changed = 1;
    while (v_changed) {
        v_changed = 0;
        {
            long long tb_end545 = ((((tb_o_Comp *)tb_nn(v_c, 1309))->q_call_to)->len - 1LL);
            for (long long v_j = 0LL; v_j <= tb_end545; v_j++) {
                if (((((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1310))->q_call_from, v_j, sizeof(long long), 1310)) >= 0LL) && tb_bget(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1310))->q_call_from, v_j, sizeof(long long), 1310)), 1310)) && (!tb_bget(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1310))->q_call_to, v_j, sizeof(long long), 1310)), 1310)))) {
                    { _Bool tb_v546 = 1; tb_bset(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1311))->q_call_to, v_j, sizeof(long long), 1311)), tb_v546, 1311); }
                    v_changed = 1;
                }
            }
        }
    }
    {
        long long tb_end547 = ((((tb_o_Comp *)tb_nn(v_c, 1313))->q_acc_field)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end547; v_j++) {
            if ((tb_bget(((tb_o_Comp *)tb_nn(v_c, 1314))->q_acc_loop, v_j, 1314) || (((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1314))->q_acc_func, v_j, sizeof(long long), 1314)) >= 0LL) && tb_bget(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1314))->q_acc_func, v_j, sizeof(long long), 1314)), 1314)))) {
                { _Bool tb_v548 = 1; _Bool *tb_p548 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1315))->q_acc_field, v_j, sizeof(tb_o_Node *), 1315)), 1315))->q_hot); *tb_p548 = tb_v548; }
            }
        }
    }
    {
        tb_list * tb_l549 = ((tb_o_Comp *)tb_nn(v_c, 1316))->q_objs;
        for (long long tb_i549 = 0; tb_i549 < tb_l549->len; tb_i549++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l549->data)[tb_i549];
            if (((tb_o_Node *)tb_nn(v_o, 1317))->q_by_cols) {
                continue;
            }
            long long v_hot = 0LL;
            long long v_cold_bytes = 0LL;
            {
                tb_list * tb_l550 = ((tb_o_Node *)tb_nn(v_o, 1321))->q_kids;
                for (long long tb_i550 = 0; tb_i550 < tb_l550->len; tb_i550++) {
                    tb_o_Node * const v_f = ((tb_o_Node * *)tb_l550->data)[tb_i550];
                    if (((tb_o_Node *)tb_nn(v_f, 1322))->q_hot) {
                        { long long tb_v551 = 1LL; long long *tb_p551 = &(v_hot); *tb_p551 += tb_v551; }
                    } else if ((((tb_o_Node *)tb_nn(v_f, 1324))->q_ty == 3LL)) {
                        { long long tb_v552 = 1LL; long long *tb_p552 = &(v_cold_bytes); *tb_p552 += tb_v552; }
                    } else {
                        { long long tb_v553 = 8LL; long long *tb_p553 = &(v_cold_bytes); *tb_p553 += tb_v553; }
                    }
                }
            }
            if (((v_hot > 0LL) && (v_cold_bytes >= 16LL))) {
                {
                    tb_list * tb_l554 = ((tb_o_Node *)tb_nn(v_o, 1329))->q_kids;
                    for (long long tb_i554 = 0; tb_i554 < tb_l554->len; tb_i554++) {
                        tb_o_Node * const v_f = ((tb_o_Node * *)tb_l554->data)[tb_i554];
                        { _Bool tb_v555 = (!((tb_o_Node *)tb_nn(v_f, 1330))->q_hot); _Bool *tb_p555 = &(((tb_o_Node *)tb_nn(v_f, 1330))->q_is_cold); *tb_p555 = tb_v555; }
                    }
                }
            }
        }
    }
}

static void f_check__check_piece_leaks(tb_o_Comp * v_c) {
    {
        tb_list * tb_l556 = ((tb_o_Comp *)tb_nn(v_c, 1334))->q_pieces;
        for (long long tb_i556 = 0; tb_i556 < tb_l556->len; tb_i556++) {
            tb_o_Node * const v_n = ((tb_o_Node * *)tb_l556->data)[tb_i556];
            long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1335))->q_kids, 1LL, sizeof(tb_o_Node *), 1335)), ((tb_o_Node *)tb_nn(v_n, 1335))->q_name);
            if ((v_bad != 0LL)) {
                { const char * tb_v557 = ((tb_o_Node *)tb_nn(v_n, 1337))->q_file; const char * *tb_p557 = &(((tb_o_Comp *)tb_nn(v_c, 1337))->q_file_name); *tb_p557 = tb_v557; }
                f_errors__error_at(v_c, v_bad, "кусок живёт только внутри своего цикла: его можно читать и менять по номеру, ходить по нему each и передавать в функцию, но нельзя сохранять, растить .add и уменьшать .pop", ((tb_o_Node *)tb_nn(v_n, 1338))->q_name);
            }
        }
    }
}

static void f_check__check_loop_body(tb_o_Comp * v_c, tb_o_Node * v_n, tb_o_Node * v_body) {
    { long long tb_v558 = 1LL; long long *tb_p558 = &(((tb_o_Comp *)tb_nn(v_c, 1343))->q_loop_depth); *tb_p558 += tb_v558; }
    if ((((tb_o_Node *)tb_nn(v_n, 1344))->q_is_par || ((tb_o_Node *)tb_nn(v_n, 1344))->q_by_vec)) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1345))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1346))->q_line, "on all cores и by vec внутри другого on all cores или by vec нельзя", "");
        }
        { long long tb_v559 = ((((tb_o_Comp *)tb_nn(v_c, 1347))->q_vars)->len - 1LL); long long *tb_p559 = &(((tb_o_Comp *)tb_nn(v_c, 1347))->q_par_base); *tb_p559 = tb_v559; }
        { tb_o_Node * tb_v560 = v_n; tb_o_Node * *tb_p560 = &(((tb_o_Comp *)tb_nn(v_c, 1348))->q_par_node); *tb_p560 = tb_v560; }
        { _Bool tb_v561 = ((tb_o_Node *)tb_nn(v_n, 1349))->q_by_vec; _Bool *tb_p561 = &(((tb_o_Comp *)tb_nn(v_c, 1349))->q_par_vec); *tb_p561 = tb_v561; }
        { long long tb_v562 = ((tb_o_Comp *)tb_nn(v_c, 1351))->q_loop_depth; long long *tb_p562 = &(((tb_o_Node *)tb_nn(v_n, 1351))->q_temp_no); *tb_p562 = tb_v562; }
        f_check__check_block(v_c, v_body);
        { long long tb_v563 = (-1LL); long long *tb_p563 = &(((tb_o_Comp *)tb_nn(v_c, 1353))->q_par_base); *tb_p563 = tb_v563; }
        { _Bool tb_v564 = 0; _Bool *tb_p564 = &(((tb_o_Comp *)tb_nn(v_c, 1354))->q_par_vec); *tb_p564 = tb_v564; }
        if (((tb_o_Node *)tb_nn(v_n, 1355))->q_by_vec) {
            {
                tb_list * tb_l565 = ((tb_o_Node *)tb_nn(v_n, 1357))->q_caps;
                for (long long tb_i565 = 0; tb_i565 < tb_l565->len; tb_i565++) {
                    tb_o_Var * const v_a = ((tb_o_Var * *)tb_l565->data)[tb_i565];
                    {
                        tb_list * tb_l566 = ((tb_o_Node *)tb_nn(v_n, 1358))->q_caps;
                        for (long long tb_i566 = 0; tb_i566 < tb_l566->len; tb_i566++) {
                            tb_o_Var * const v_b = ((tb_o_Var * *)tb_l566->data)[tb_i566];
                            if ((((strcmp(((tb_o_Var *)tb_nn(v_a, 1359))->q_name, ((tb_o_Var *)tb_nn(v_b, 1359))->q_name) == 0) && ((tb_o_Var *)tb_nn(v_a, 1359))->q_is_shared) && (!((tb_o_Var *)tb_nn(v_b, 1359))->q_is_shared))) {
                                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1360))->q_line, "в by vec переменную, которую копишь через += или -=, нельзя читать внутри цикла", ((tb_o_Var *)tb_nn(v_a, 1360))->q_name);
                            }
                        }
                    }
                }
            }
        }
    } else {
        f_check__check_block(v_c, v_body);
    }
    { long long tb_v567 = 1LL; long long *tb_p567 = &(((tb_o_Comp *)tb_nn(v_c, 1363))->q_loop_depth); *tb_p567 -= tb_v567; }
}

static void f_check__check_block(tb_o_Comp * v_c, tb_o_Node * v_block) {
    f_check__open_scope(v_c);
    {
        tb_list * tb_l568 = ((tb_o_Node *)tb_nn(v_block, 1367))->q_kids;
        for (long long tb_i568 = 0; tb_i568 < tb_l568->len; tb_i568++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l568->data)[tb_i568];
            f_check__check_statement(v_c, v_s);
        }
    }
    f_check__close_scope(v_c);
}

static void f_check__check_func(tb_o_Comp * v_c, long long v_fi) {
    tb_o_Func * v_f = (*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1372))->q_funcs, v_fi, sizeof(tb_o_Func *), 1372));
    { long long tb_v569 = v_fi; long long *tb_p569 = &(((tb_o_Comp *)tb_nn(v_c, 1373))->q_current); *tb_p569 = tb_v569; }
    { const char * tb_v570 = ((tb_o_Func *)tb_nn(v_f, 1374))->q_mod; const char * *tb_p570 = &(((tb_o_Comp *)tb_nn(v_c, 1374))->q_cur_mod); *tb_p570 = tb_v570; }
    { const char * tb_v571 = ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1375))->q_node, 1375))->q_file; const char * *tb_p571 = &(((tb_o_Comp *)tb_nn(v_c, 1375))->q_file_name); *tb_p571 = tb_v571; }
    { long long tb_v572 = 1LL; long long *tb_p572 = &(((tb_o_Func *)tb_nn(v_f, 1376))->q_state); *tb_p572 = tb_v572; }
    { long long tb_v573 = 0LL; long long *tb_p573 = &(((tb_o_Comp *)tb_nn(v_c, 1377))->q_loop_depth); *tb_p573 = tb_v573; }
    f_check__open_scope(v_c);
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1379))->q_node, 1379))->q_kids, 0LL, sizeof(tb_o_Node *), 1379));
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1380))->q_node, 1380))->q_kids, 2LL, sizeof(tb_o_Node *), 1380));
    {
        tb_list * tb_l574 = ((tb_o_Node *)tb_nn(v_params, 1381))->q_kids;
        for (long long tb_i574 = 0; tb_i574 < tb_l574->len; tb_i574++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l574->data)[tb_i574];
            f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_p, 1382))->q_name, ((tb_o_Node *)tb_nn(v_p, 1382))->q_ty, 0, ((tb_o_Node *)tb_nn(v_p, 1382))->q_is_link, ((tb_o_Node *)tb_nn(v_p, 1382))->q_line);
        }
    }
    {
        tb_list * tb_l575 = ((tb_o_Node *)tb_nn(v_outputs, 1383))->q_kids;
        for (long long tb_i575 = 0; tb_i575 < tb_l575->len; tb_i575++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l575->data)[tb_i575];
            long long v_same_param = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_o, 1384))->q_name);
            if ((v_same_param >= 0LL)) {
                if ((((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1387))->q_vars, v_same_param, sizeof(tb_o_Var *), 1387)), 1387))->q_type != ((tb_o_Node *)tb_nn(v_o, 1387))->q_ty)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1388))->q_line, "выход и параметр с одним именем должны быть одного типа", ((tb_o_Node *)tb_nn(v_o, 1388))->q_name);
                }
                { _Bool tb_v576 = 1; _Bool *tb_p576 = &(((tb_o_Node *)tb_nn(v_o, 1390))->q_is_const); *tb_p576 = tb_v576; }
            } else {
                f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_o, 1392))->q_name, ((tb_o_Node *)tb_nn(v_o, 1392))->q_ty, 0, 0, ((tb_o_Node *)tb_nn(v_o, 1392))->q_line);
            }
        }
    }
    f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1393))->q_node, 1393))->q_kids, 1LL, sizeof(tb_o_Node *), 1393)));
    f_check__close_scope(v_c);
    if ((((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1395))->q_node, 1395))->q_ty < 0LL)) {
        { long long tb_v577 = 0LL; long long *tb_p577 = &(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1396))->q_node, 1396))->q_ty); *tb_p577 = tb_v577; }
    }
    { long long tb_v578 = 2LL; long long *tb_p578 = &(((tb_o_Func *)tb_nn(v_f, 1397))->q_state); *tb_p578 = tb_v578; }
}

static void f_check__register_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_o, 1400))->q_name) >= 0LL) || f_check__is_builtin(((tb_o_Node *)tb_nn(v_o, 1400))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1401))->q_line, "это имя уже занято", ((tb_o_Node *)tb_nn(v_o, 1401))->q_name);
    }
    if (((((tb_o_Comp *)tb_nn(v_c, 1402))->q_objs)->len == 500LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1403))->q_line, "слишком много obj", "");
    }
    { tb_o_Node * tb_v579 = v_o; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1404))->q_objs) = tb_v579; }
}

static void f_check__register_func(tb_o_Comp * v_c, tb_o_Node * v_fn) {
    if ((((f_check__find_func_in(v_c, ((tb_o_Node *)tb_nn(v_fn, 1407))->q_name, ((tb_o_Node *)tb_nn(v_fn, 1407))->q_mod) >= 0LL) || (f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_fn, 1407))->q_name) >= 0LL)) || f_check__is_builtin(((tb_o_Node *)tb_nn(v_fn, 1407))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_fn, 1408))->q_line, "это имя уже занято", ((tb_o_Node *)tb_nn(v_fn, 1408))->q_name);
    }
    if (((((tb_o_Comp *)tb_nn(v_c, 1409))->q_funcs)->len == 1000LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_fn, 1410))->q_line, "слишком много функций", "");
    }
    { tb_o_Func * tb_v580 = tb_new_Func(((tb_o_Node *)tb_nn(v_fn, 1411))->q_name, ((tb_o_Node *)tb_nn(v_fn, 1411))->q_mod, v_fn, 0LL); *(tb_o_Func * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1411))->q_funcs) = tb_v580; }
}

static _Bool f_check__has_fail(tb_o_Node * v_n) {
    _Bool v_r = 0;
    if ((({ const char *tb_s581 = ((tb_o_Node *)tb_nn(v_n, 1415))->q_kind; ((unsigned char)tb_s581[0] == 102 && (unsigned char)tb_s581[1] == 97 && (unsigned char)tb_s581[2] == 105 && (unsigned char)tb_s581[3] == 108 && tb_s581[4] == 0); }) || ({ const char *tb_s582 = ((tb_o_Node *)tb_nn(v_n, 1415))->q_kind; ((unsigned char)tb_s582[0] == 116 && (unsigned char)tb_s582[1] == 114 && (unsigned char)tb_s582[2] == 121 && tb_s582[3] == 0); }))) {
        return 1;
    }
    {
        tb_list * tb_l583 = ((tb_o_Node *)tb_nn(v_n, 1417))->q_kids;
        for (long long tb_i583 = 0; tb_i583 < tb_l583->len; tb_i583++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l583->data)[tb_i583];
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
    if ((({ const char *tb_s584 = ((tb_o_Node *)tb_nn(v_n, 1424))->q_kind; ((unsigned char)tb_s584[0] == 114 && (unsigned char)tb_s584[1] == 101 && (unsigned char)tb_s584[2] == 116 && (unsigned char)tb_s584[3] == 117 && (unsigned char)tb_s584[4] == 114 && (unsigned char)tb_s584[5] == 110 && tb_s584[6] == 0); }) && ((((tb_o_Node *)tb_nn(v_n, 1424))->q_kids)->len > 0LL))) {
        return 1;
    }
    {
        tb_list * tb_l585 = ((tb_o_Node *)tb_nn(v_n, 1426))->q_kids;
        for (long long tb_i585 = 0; tb_i585 < tb_l585->len; tb_i585++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l585->data)[tb_i585];
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
        tb_list * tb_l586 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_fn, 1432))->q_kids, 0LL, sizeof(tb_o_Node *), 1432)), 1432))->q_kids;
        for (long long tb_i586 = 0; tb_i586 < tb_l586->len; tb_i586++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l586->data)[tb_i586];
            if ((((tb_o_Node *)tb_nn(v_p, 1433))->q_ty < 0LL)) {
                return 0;
            }
        }
    }
    return 1;
    tb_crash(1431, "функция links_known дошла до конца без return");
    return 0;
}

static void f_check__resolve_params(tb_o_Comp * v_c, tb_o_Node * v_params) {
    {
        long long tb_end587 = ((((tb_o_Node *)tb_nn(v_params, 1438))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end587; v_i++) {
            tb_o_Node * v_p = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1439))->q_kids, v_i, sizeof(tb_o_Node *), 1439));
            {
                long long tb_end588 = (v_i - 1LL);
                for (long long v_j = 0LL; v_j <= tb_end588; v_j++) {
                    if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1441))->q_kids, v_j, sizeof(tb_o_Node *), 1441)), 1441))->q_name, ((tb_o_Node *)tb_nn(v_p, 1441))->q_name) == 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_p, 1442))->q_line, "это имя уже есть в списке", ((tb_o_Node *)tb_nn(v_p, 1442))->q_name);
                    }
                }
            }
            if (((tb_o_Node *)tb_nn(v_p, 1443))->q_has_tsyn) {
                { long long tb_v589 = f_check__resolve_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1444))->q_tsyn); long long *tb_p589 = &(((tb_o_Node *)tb_nn(v_p, 1444))->q_ty); *tb_p589 = tb_v589; }
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
    { long long tb_v590 = (-1LL); *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1452))->q_temp_types) = tb_v590; }
    { long long tb_v591 = (-1LL); long long *tb_p591 = &(((tb_o_Comp *)tb_nn(v_c, 1453))->q_par_base); *tb_p591 = tb_v591; }
    { long long tb_v592 = (-1LL); long long *tb_p592 = &(((tb_o_Comp *)tb_nn(v_c, 1454))->q_temp_base); *tb_p592 = tb_v592; }
    const char * v_main_mod = "";
    if (((((tb_o_Node *)tb_nn(v_program, 1458))->q_kids)->len > 0LL)) {
        v_main_mod = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1459))->q_kids, 0LL, sizeof(tb_o_Node *), 1459)), 1459))->q_mod;
    }
    {
        tb_list * tb_l593 = ((tb_o_Node *)tb_nn(v_program, 1460))->q_kids;
        for (long long tb_i593 = 0; tb_i593 < tb_l593->len; tb_i593++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l593->data)[tb_i593];
            if ((!f_check__is_module(v_c, ((tb_o_Node *)tb_nn(v_top, 1461))->q_mod))) {
                { const char * tb_v594 = ((tb_o_Node *)tb_nn(v_top, 1462))->q_mod; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1462))->q_all_mods) = tb_v594; }
            }
            if (({ const char *tb_s595 = ((tb_o_Node *)tb_nn(v_top, 1463))->q_kind; ((unsigned char)tb_s595[0] == 117 && (unsigned char)tb_s595[1] == 115 && (unsigned char)tb_s595[2] == 101 && tb_s595[3] == 0); })) {
                if (((((tb_o_Comp *)tb_nn(v_c, 1464))->q_uses)->len == 500LL)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_top, 1465))->q_line, "слишком много use", "");
                }
                { tb_o_Use * tb_v596 = tb_new_Use(((tb_o_Node *)tb_nn(v_top, 1466))->q_mod, ((tb_o_Node *)tb_nn(v_top, 1466))->q_name); *(tb_o_Use * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1466))->q_uses) = tb_v596; }
            }
        }
    }
    {
        tb_list * tb_l597 = ((tb_o_Node *)tb_nn(v_program, 1468))->q_kids;
        for (long long tb_i597 = 0; tb_i597 < tb_l597->len; tb_i597++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l597->data)[tb_i597];
            { const char * tb_v598 = ((tb_o_Node *)tb_nn(v_top, 1469))->q_file; const char * *tb_p598 = &(((tb_o_Comp *)tb_nn(v_c, 1469))->q_file_name); *tb_p598 = tb_v598; }
            if (({ const char *tb_s599 = ((tb_o_Node *)tb_nn(v_top, 1470))->q_kind; ((unsigned char)tb_s599[0] == 111 && (unsigned char)tb_s599[1] == 98 && (unsigned char)tb_s599[2] == 106 && tb_s599[3] == 0); })) {
                f_check__register_obj(v_c, v_top);
            } else if (({ const char *tb_s600 = ((tb_o_Node *)tb_nn(v_top, 1472))->q_kind; ((unsigned char)tb_s600[0] == 102 && (unsigned char)tb_s600[1] == 117 && (unsigned char)tb_s600[2] == 110 && (unsigned char)tb_s600[3] == 99 && tb_s600[4] == 0); })) {
                f_check__register_func(v_c, v_top);
                { _Bool tb_v601 = f_check__has_fail((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1474))->q_kids, 1LL, sizeof(tb_o_Node *), 1474))); _Bool *tb_p601 = &(((tb_o_Node *)tb_nn(v_top, 1474))->q_can_fail); *tb_p601 = tb_v601; }
                if ((({ const char *tb_s602 = ((tb_o_Node *)tb_nn(v_top, 1475))->q_name; ((unsigned char)tb_s602[0] == 109 && (unsigned char)tb_s602[1] == 97 && (unsigned char)tb_s602[2] == 105 && (unsigned char)tb_s602[3] == 110 && tb_s602[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_top, 1475))->q_mod, v_main_mod) != 0))) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_top, 1476))->q_line, "main бывает только в главном файле, в модуле её быть не должно", "");
                }
            }
        }
    }
    {
        tb_list * tb_l603 = ((tb_o_Node *)tb_nn(v_program, 1478))->q_kids;
        for (long long tb_i603 = 0; tb_i603 < tb_l603->len; tb_i603++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l603->data)[tb_i603];
            { const char * tb_v604 = ((tb_o_Node *)tb_nn(v_top, 1479))->q_file; const char * *tb_p604 = &(((tb_o_Comp *)tb_nn(v_c, 1479))->q_file_name); *tb_p604 = tb_v604; }
            if (({ const char *tb_s605 = ((tb_o_Node *)tb_nn(v_top, 1480))->q_kind; ((unsigned char)tb_s605[0] == 111 && (unsigned char)tb_s605[1] == 98 && (unsigned char)tb_s605[2] == 106 && tb_s605[3] == 0); })) {
                f_check__resolve_params(v_c, v_top);
            } else if (({ const char *tb_s606 = ((tb_o_Node *)tb_nn(v_top, 1482))->q_kind; ((unsigned char)tb_s606[0] == 102 && (unsigned char)tb_s606[1] == 117 && (unsigned char)tb_s606[2] == 110 && (unsigned char)tb_s606[3] == 99 && tb_s606[4] == 0); })) {
                f_check__resolve_params(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1483))->q_kids, 0LL, sizeof(tb_o_Node *), 1483)));
                f_check__resolve_params(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1484))->q_kids, 2LL, sizeof(tb_o_Node *), 1484)));
                tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1485))->q_kids, 2LL, sizeof(tb_o_Node *), 1485));
                if (((((tb_o_Node *)tb_nn(v_outputs, 1486))->q_kids)->len == 1LL)) {
                    { long long tb_v607 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1487))->q_kids, 0LL, sizeof(tb_o_Node *), 1487)), 1487))->q_ty; long long *tb_p607 = &(((tb_o_Node *)tb_nn(v_top, 1487))->q_ty); *tb_p607 = tb_v607; }
                } else if (((((tb_o_Node *)tb_nn(v_outputs, 1488))->q_kids)->len > 1LL)) {
                    { long long tb_v608 = f_check__multi_type(v_c, v_top); long long *tb_p608 = &(((tb_o_Node *)tb_nn(v_top, 1489))->q_ty); *tb_p608 = tb_v608; }
                } else if ((!f_check__has_value_return((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1490))->q_kids, 1LL, sizeof(tb_o_Node *), 1490))))) {
                    { long long tb_v609 = 0LL; long long *tb_p609 = &(((tb_o_Node *)tb_nn(v_top, 1492))->q_ty); *tb_p609 = tb_v609; }
                }
            }
        }
    }
    long long v_main_func = f_check__find_func_in(v_c, "main", v_main_mod);
    if (((((tb_o_Node *)tb_nn(v_program, 1494))->q_kids)->len > 0LL)) {
        { const char * tb_v610 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1495))->q_kids, 0LL, sizeof(tb_o_Node *), 1495)), 1495))->q_file; const char * *tb_p610 = &(((tb_o_Comp *)tb_nn(v_c, 1495))->q_file_name); *tb_p610 = tb_v610; }
    }
    if ((v_main_func < 0LL)) {
        f_errors__error_at(v_c, 0LL, "нет функции main: программа начинается с func main()", "");
    }
    tb_o_Node * v_main_node = ((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1498))->q_funcs, v_main_func, sizeof(tb_o_Func *), 1498)), 1498))->q_node;
    if ((((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_main_node, 1499))->q_kids, 0LL, sizeof(tb_o_Node *), 1499)), 1499))->q_kids)->len != 0LL) || ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_main_node, 1499))->q_kids, 2LL, sizeof(tb_o_Node *), 1499)), 1499))->q_kids)->len != 0LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_main_node, 1500))->q_line, "у main не бывает параметров и gives", "");
    }
    f_check__check_func(v_c, v_main_func);
    _Bool v_progress = 1;
    while (v_progress) {
        v_progress = 0;
        {
            long long tb_end611 = ((((tb_o_Comp *)tb_nn(v_c, 1507))->q_funcs)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end611; v_i++) {
                if (((((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1508))->q_funcs, v_i, sizeof(tb_o_Func *), 1508)), 1508))->q_state != 0LL) || (!f_check__links_known(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1508))->q_funcs, v_i, sizeof(tb_o_Func *), 1508)), 1508))->q_node)))) {
                    continue;
                }
                f_check__cut_vars(v_c, 0LL);
                { long long tb_v612 = 0LL; long long *tb_p612 = &(((tb_o_Comp *)tb_nn(v_c, 1511))->q_depth); *tb_p612 = tb_v612; }
                { long long tb_v613 = 0LL; long long *tb_p613 = &(((tb_o_Comp *)tb_nn(v_c, 1512))->q_var_base); *tb_p613 = tb_v613; }
                f_check__check_func(v_c, v_i);
                v_progress = 1;
            }
        }
    }
    {
        tb_list * tb_l614 = ((tb_o_Comp *)tb_nn(v_c, 1515))->q_funcs;
        for (long long tb_i614 = 0; tb_i614 < tb_l614->len; tb_i614++) {
            tb_o_Func * const v_f = ((tb_o_Func * *)tb_l614->data)[tb_i614];
            if ((((tb_o_Func *)tb_nn(v_f, 1516))->q_state == 0LL)) {
                { const char * tb_v615 = ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1517))->q_node, 1517))->q_file; const char * *tb_p615 = &(((tb_o_Comp *)tb_nn(v_c, 1517))->q_file_name); *tb_p615 = tb_v615; }
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1518))->q_node, 1518))->q_line, "функция с link ни разу не вызвана, поэтому тип ярлыка неизвестен", ((tb_o_Func *)tb_nn(v_f, 1518))->q_name);
            }
        }
    }
    f_check__check_piece_leaks(v_c);
    f_check__mark_hot_fields(v_c);
}

static void f_emit__put(tb_o_Comp * v_c, const char * v_s) {
    { const char * tb_v616 = v_s; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 10))->q_out) = tb_v616; }
}

static const char * f_emit__fname(tb_o_Node * v_f) {
    long long tb_rl_mod = -1, tb_rc_mod = 0; (void)tb_rl_mod; (void)tb_rc_mod;
    const char * v_mod = ""; tb_rl_mod = -1;
    {
        const char * tb_l617 = ((tb_o_Node *)tb_nn(v_f, 15))->q_mod;
        for (long long tb_i617 = 0; tb_l617[tb_i617] != 0; tb_i617++) {
            const char * const v_ch = tb_chr((unsigned char)tb_l617[tb_i617]);
            long long v_k = tb_code(v_ch, 16);
            if (((((v_k >= 97LL) && (v_k <= 122LL)) || ((v_k >= 65LL) && (v_k <= 90LL))) || ((v_k >= 48LL) && (v_k <= 57LL)))) {
                tb_app(&v_mod, &tb_rl_mod, &tb_rc_mod, v_ch); 
            } else {
                tb_app_n(&v_mod, &tb_rl_mod, &tb_rc_mod, "_", sizeof("_") - 1); 
            }
        }
    }
    return ({ const char *tb_c618_0 = "f_"; long long tb_c618_0n = sizeof("f_") - 1; const char *tb_c618_1 = v_mod; long long tb_c618_1n = (long long)strlen(tb_c618_1); const char *tb_c618_2 = "__"; long long tb_c618_2n = sizeof("__") - 1; const char *tb_c618_3 = ((tb_o_Node *)tb_nn(v_f, 21))->q_name; long long tb_c618_3n = (long long)strlen(tb_c618_3); char *tb_c618r = tb_mem(tb_c618_0n + tb_c618_1n + tb_c618_2n + tb_c618_3n + 1), *tb_c618rw = tb_c618r; memcpy(tb_c618rw, tb_c618_0, tb_c618_0n); tb_c618rw += tb_c618_0n; memcpy(tb_c618rw, tb_c618_1, tb_c618_1n); tb_c618rw += tb_c618_1n; memcpy(tb_c618rw, tb_c618_2, tb_c618_2n); tb_c618rw += tb_c618_2n; memcpy(tb_c618rw, tb_c618_3, tb_c618_3n); tb_c618rw += tb_c618_3n; *tb_c618rw = 0; (const char *)tb_c618r; });
    tb_crash(13, "функция fname дошла до конца без return");
    return "";
}

static const char * f_emit__c_type(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    if (({ const char *tb_s619 = v_k; ((unsigned char)tb_s619[0] == 105 && (unsigned char)tb_s619[1] == 110 && (unsigned char)tb_s619[2] == 116 && tb_s619[3] == 0); })) {
        return "long long";
    }
    if (({ const char *tb_s620 = v_k; ((unsigned char)tb_s620[0] == 100 && (unsigned char)tb_s620[1] == 101 && (unsigned char)tb_s620[2] == 99 && tb_s620[3] == 0); })) {
        return "double";
    }
    if (({ const char *tb_s621 = v_k; ((unsigned char)tb_s621[0] == 98 && (unsigned char)tb_s621[1] == 111 && (unsigned char)tb_s621[2] == 111 && (unsigned char)tb_s621[3] == 108 && tb_s621[4] == 0); })) {
        return "_Bool";
    }
    if (({ const char *tb_s622 = v_k; ((unsigned char)tb_s622[0] == 116 && (unsigned char)tb_s622[1] == 101 && (unsigned char)tb_s622[2] == 120 && (unsigned char)tb_s622[3] == 116 && tb_s622[4] == 0); })) {
        return "const char *";
    }
    if (({ const char *tb_s623 = v_k; ((unsigned char)tb_s623[0] == 108 && (unsigned char)tb_s623[1] == 105 && (unsigned char)tb_s623[2] == 115 && (unsigned char)tb_s623[3] == 116 && tb_s623[4] == 0); })) {
        if (f_check__is_cols_list(v_c, v_t)) {
            return ({ const char *tb_c624_0 = "tb_cols_"; long long tb_c624_0n = sizeof("tb_cols_") - 1; const char *tb_c624_1 = ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 35))->q_types, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 35))->q_types, v_t, sizeof(tb_o_Type *), 35)), 35))->q_elem, sizeof(tb_o_Type *), 35)), 35))->q_obj_node, 35))->q_name; long long tb_c624_1n = (long long)strlen(tb_c624_1); const char *tb_c624_2 = " *"; long long tb_c624_2n = sizeof(" *") - 1; char *tb_c624r = tb_mem(tb_c624_0n + tb_c624_1n + tb_c624_2n + 1), *tb_c624rw = tb_c624r; memcpy(tb_c624rw, tb_c624_0, tb_c624_0n); tb_c624rw += tb_c624_0n; memcpy(tb_c624rw, tb_c624_1, tb_c624_1n); tb_c624rw += tb_c624_1n; memcpy(tb_c624rw, tb_c624_2, tb_c624_2n); tb_c624rw += tb_c624_2n; *tb_c624rw = 0; (const char *)tb_c624r; });
        }
        return "tb_list *";
    }
    if (({ const char *tb_s625 = v_k; ((unsigned char)tb_s625[0] == 100 && (unsigned char)tb_s625[1] == 105 && (unsigned char)tb_s625[2] == 99 && (unsigned char)tb_s625[3] == 116 && tb_s625[4] == 0); })) {
        return "tb_dict *";
    }
    if (({ const char *tb_s626 = v_k; ((unsigned char)tb_s626[0] == 111 && (unsigned char)tb_s626[1] == 98 && (unsigned char)tb_s626[2] == 106 && tb_s626[3] == 0); })) {
        if (f_check__is_cols_obj(v_c, v_t)) {
            return ({ const char *tb_c627_0 = "tb_row_"; long long tb_c627_0n = sizeof("tb_row_") - 1; const char *tb_c627_1 = ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 41))->q_types, v_t, sizeof(tb_o_Type *), 41)), 41))->q_obj_node, 41))->q_name; long long tb_c627_1n = (long long)strlen(tb_c627_1); char *tb_c627r = tb_mem(tb_c627_0n + tb_c627_1n + 1), *tb_c627rw = tb_c627r; memcpy(tb_c627rw, tb_c627_0, tb_c627_0n); tb_c627rw += tb_c627_0n; memcpy(tb_c627rw, tb_c627_1, tb_c627_1n); tb_c627rw += tb_c627_1n; *tb_c627rw = 0; (const char *)tb_c627r; });
        }
        return ({ const char *tb_c628_0 = "tb_o_"; long long tb_c628_0n = sizeof("tb_o_") - 1; const char *tb_c628_1 = ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 42))->q_types, v_t, sizeof(tb_o_Type *), 42)), 42))->q_obj_node, 42))->q_name; long long tb_c628_1n = (long long)strlen(tb_c628_1); const char *tb_c628_2 = " *"; long long tb_c628_2n = sizeof(" *") - 1; char *tb_c628r = tb_mem(tb_c628_0n + tb_c628_1n + tb_c628_2n + 1), *tb_c628rw = tb_c628r; memcpy(tb_c628rw, tb_c628_0, tb_c628_0n); tb_c628rw += tb_c628_0n; memcpy(tb_c628rw, tb_c628_1, tb_c628_1n); tb_c628rw += tb_c628_1n; memcpy(tb_c628rw, tb_c628_2, tb_c628_2n); tb_c628rw += tb_c628_2n; *tb_c628rw = 0; (const char *)tb_c628r; });
    }
    if (({ const char *tb_s629 = v_k; ((unsigned char)tb_s629[0] == 109 && (unsigned char)tb_s629[1] == 117 && (unsigned char)tb_s629[2] == 108 && (unsigned char)tb_s629[3] == 116 && (unsigned char)tb_s629[4] == 105 && tb_s629[5] == 0); })) {
        return ({ const char *tb_c630_0 = "tb_r_"; long long tb_c630_0n = sizeof("tb_r_") - 1; const char *tb_c630_1 = f_emit__fname(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 44))->q_types, v_t, sizeof(tb_o_Type *), 44)), 44))->q_func_node); long long tb_c630_1n = (long long)strlen(tb_c630_1); char *tb_c630r = tb_mem(tb_c630_0n + tb_c630_1n + 1), *tb_c630rw = tb_c630r; memcpy(tb_c630rw, tb_c630_0, tb_c630_0n); tb_c630rw += tb_c630_0n; memcpy(tb_c630rw, tb_c630_1, tb_c630_1n); tb_c630rw += tb_c630_1n; *tb_c630rw = 0; (const char *)tb_c630r; });
    }
    if (({ const char *tb_s631 = v_k; ((unsigned char)tb_s631[0] == 116 && (unsigned char)tb_s631[1] == 97 && (unsigned char)tb_s631[2] == 115 && (unsigned char)tb_s631[3] == 107 && tb_s631[4] == 0); })) {
        return "tb_task *";
    }
    return "void";
    tb_crash(23, "функция c_type дошла до конца без return");
    return "";
}

static void f_emit__indent(tb_o_Comp * v_c, long long v_depth) {
    {
        long long tb_end632 = v_depth;
        for (long long v_i = 1LL; v_i <= tb_end632; v_i++) {
            f_emit__put(v_c, "    ");
        }
    }
}

static _Bool f_emit__is_bits(tb_o_Comp * v_c, long long v_t) {
    return (({ const char *tb_s633 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s633[0] == 108 && (unsigned char)tb_s633[1] == 105 && (unsigned char)tb_s633[2] == 115 && (unsigned char)tb_s633[3] == 116 && tb_s633[4] == 0); }) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 55))->q_types, v_t, sizeof(tb_o_Type *), 55)), 55))->q_elem == 3LL));
    tb_crash(54, "функция is_bits дошла до конца без return");
    return 0;
}

static void f_emit__emit_default(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    if (({ const char *tb_s634 = v_k; ((unsigned char)tb_s634[0] == 105 && (unsigned char)tb_s634[1] == 110 && (unsigned char)tb_s634[2] == 116 && tb_s634[3] == 0); })) {
        f_emit__put(v_c, "0LL");
    } else if (({ const char *tb_s635 = v_k; ((unsigned char)tb_s635[0] == 100 && (unsigned char)tb_s635[1] == 101 && (unsigned char)tb_s635[2] == 99 && tb_s635[3] == 0); })) {
        f_emit__put(v_c, "0.0");
    } else if (({ const char *tb_s636 = v_k; ((unsigned char)tb_s636[0] == 116 && (unsigned char)tb_s636[1] == 101 && (unsigned char)tb_s636[2] == 120 && (unsigned char)tb_s636[3] == 116 && tb_s636[4] == 0); })) {
        f_emit__put(v_c, "\"\"");
    } else if ((({ const char *tb_s637 = v_k; ((unsigned char)tb_s637[0] == 108 && (unsigned char)tb_s637[1] == 105 && (unsigned char)tb_s637[2] == 115 && (unsigned char)tb_s637[3] == 116 && tb_s637[4] == 0); }) && f_check__is_cols_list(v_c, v_t))) {
        f_emit__put(v_c, ({ const char *tb_c638_0 = "tb_cols_new_"; long long tb_c638_0n = sizeof("tb_cols_new_") - 1; const char *tb_c638_1 = f_emit__cols_name(v_c, v_t); long long tb_c638_1n = (long long)strlen(tb_c638_1); const char *tb_c638_2 = "()"; long long tb_c638_2n = sizeof("()") - 1; char *tb_c638r = tb_mem(tb_c638_0n + tb_c638_1n + tb_c638_2n + 1), *tb_c638rw = tb_c638r; memcpy(tb_c638rw, tb_c638_0, tb_c638_0n); tb_c638rw += tb_c638_0n; memcpy(tb_c638rw, tb_c638_1, tb_c638_1n); tb_c638rw += tb_c638_1n; memcpy(tb_c638rw, tb_c638_2, tb_c638_2n); tb_c638rw += tb_c638_2n; *tb_c638rw = 0; (const char *)tb_c638r; }));
    } else if ((({ const char *tb_s639 = v_k; ((unsigned char)tb_s639[0] == 111 && (unsigned char)tb_s639[1] == 98 && (unsigned char)tb_s639[2] == 106 && tb_s639[3] == 0); }) && f_check__is_cols_obj(v_c, v_t))) {
        f_emit__put(v_c, ({ const char *tb_c640_0 = "("; long long tb_c640_0n = sizeof("(") - 1; const char *tb_c640_1 = f_emit__c_type(v_c, v_t); long long tb_c640_1n = (long long)strlen(tb_c640_1); const char *tb_c640_2 = "){0, 0}"; long long tb_c640_2n = sizeof("){0, 0}") - 1; char *tb_c640r = tb_mem(tb_c640_0n + tb_c640_1n + tb_c640_2n + 1), *tb_c640rw = tb_c640r; memcpy(tb_c640rw, tb_c640_0, tb_c640_0n); tb_c640rw += tb_c640_0n; memcpy(tb_c640rw, tb_c640_1, tb_c640_1n); tb_c640rw += tb_c640_1n; memcpy(tb_c640rw, tb_c640_2, tb_c640_2n); tb_c640rw += tb_c640_2n; *tb_c640rw = 0; (const char *)tb_c640r; }));
    } else if ((({ const char *tb_s641 = v_k; ((unsigned char)tb_s641[0] == 108 && (unsigned char)tb_s641[1] == 105 && (unsigned char)tb_s641[2] == 115 && (unsigned char)tb_s641[3] == 116 && tb_s641[4] == 0); }) && f_emit__is_bits(v_c, v_t))) {
        f_emit__put(v_c, "tb_bits_new()");
    } else if (({ const char *tb_s642 = v_k; ((unsigned char)tb_s642[0] == 108 && (unsigned char)tb_s642[1] == 105 && (unsigned char)tb_s642[2] == 115 && (unsigned char)tb_s642[3] == 116 && tb_s642[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c643_0 = "tb_list_new(sizeof("; long long tb_c643_0n = sizeof("tb_list_new(sizeof(") - 1; const char *tb_c643_1 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 72))->q_types, v_t, sizeof(tb_o_Type *), 72)), 72))->q_elem); long long tb_c643_1n = (long long)strlen(tb_c643_1); const char *tb_c643_2 = "))"; long long tb_c643_2n = sizeof("))") - 1; char *tb_c643r = tb_mem(tb_c643_0n + tb_c643_1n + tb_c643_2n + 1), *tb_c643rw = tb_c643r; memcpy(tb_c643rw, tb_c643_0, tb_c643_0n); tb_c643rw += tb_c643_0n; memcpy(tb_c643rw, tb_c643_1, tb_c643_1n); tb_c643rw += tb_c643_1n; memcpy(tb_c643rw, tb_c643_2, tb_c643_2n); tb_c643rw += tb_c643_2n; *tb_c643rw = 0; (const char *)tb_c643r; }));
    } else if (({ const char *tb_s644 = v_k; ((unsigned char)tb_s644[0] == 100 && (unsigned char)tb_s644[1] == 105 && (unsigned char)tb_s644[2] == 99 && (unsigned char)tb_s644[3] == 116 && tb_s644[4] == 0); })) {
        const char * v_is_text = "0";
        if (({ const char *tb_s645 = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 75))->q_types, v_t, sizeof(tb_o_Type *), 75)), 75))->q_key); ((unsigned char)tb_s645[0] == 116 && (unsigned char)tb_s645[1] == 101 && (unsigned char)tb_s645[2] == 120 && (unsigned char)tb_s645[3] == 116 && tb_s645[4] == 0); })) {
            v_is_text = "1";
        }
        f_emit__put(v_c, ({ const char *tb_c646_0 = "tb_dict_new("; long long tb_c646_0n = sizeof("tb_dict_new(") - 1; const char *tb_c646_1 = v_is_text; long long tb_c646_1n = (long long)strlen(tb_c646_1); const char *tb_c646_2 = ", sizeof("; long long tb_c646_2n = sizeof(", sizeof(") - 1; const char *tb_c646_3 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 77))->q_types, v_t, sizeof(tb_o_Type *), 77)), 77))->q_elem); long long tb_c646_3n = (long long)strlen(tb_c646_3); const char *tb_c646_4 = "))"; long long tb_c646_4n = sizeof("))") - 1; char *tb_c646r = tb_mem(tb_c646_0n + tb_c646_1n + tb_c646_2n + tb_c646_3n + tb_c646_4n + 1), *tb_c646rw = tb_c646r; memcpy(tb_c646rw, tb_c646_0, tb_c646_0n); tb_c646rw += tb_c646_0n; memcpy(tb_c646rw, tb_c646_1, tb_c646_1n); tb_c646rw += tb_c646_1n; memcpy(tb_c646rw, tb_c646_2, tb_c646_2n); tb_c646rw += tb_c646_2n; memcpy(tb_c646rw, tb_c646_3, tb_c646_3n); tb_c646rw += tb_c646_3n; memcpy(tb_c646rw, tb_c646_4, tb_c646_4n); tb_c646rw += tb_c646_4n; *tb_c646rw = 0; (const char *)tb_c646r; }));
    } else {
        f_emit__put(v_c, "0");
    }
}

static const char * f_emit__cols_name(tb_o_Comp * v_c, long long v_t) {
    if (({ const char *tb_s647 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s647[0] == 108 && (unsigned char)tb_s647[1] == 105 && (unsigned char)tb_s647[2] == 115 && (unsigned char)tb_s647[3] == 116 && tb_s647[4] == 0); })) {
        return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 84))->q_types, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 84))->q_types, v_t, sizeof(tb_o_Type *), 84)), 84))->q_elem, sizeof(tb_o_Type *), 84)), 84))->q_obj_node, 84))->q_name;
    }
    return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 85))->q_types, v_t, sizeof(tb_o_Type *), 85)), 85))->q_obj_node, 85))->q_name;
    tb_crash(82, "функция cols_name дошла до конца без return");
    return "";
}

static _Bool f_emit__is_simple(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    return (((({ const char *tb_s648 = v_k; ((unsigned char)tb_s648[0] == 105 && (unsigned char)tb_s648[1] == 110 && (unsigned char)tb_s648[2] == 116 && tb_s648[3] == 0); }) || ({ const char *tb_s649 = v_k; ((unsigned char)tb_s649[0] == 100 && (unsigned char)tb_s649[1] == 101 && (unsigned char)tb_s649[2] == 99 && tb_s649[3] == 0); })) || ({ const char *tb_s650 = v_k; ((unsigned char)tb_s650[0] == 98 && (unsigned char)tb_s650[1] == 111 && (unsigned char)tb_s650[2] == 111 && (unsigned char)tb_s650[3] == 108 && tb_s650[4] == 0); })) || ({ const char *tb_s651 = v_k; ((unsigned char)tb_s651[0] == 116 && (unsigned char)tb_s651[1] == 101 && (unsigned char)tb_s651[2] == 120 && (unsigned char)tb_s651[3] == 116 && tb_s651[4] == 0); }));
    tb_crash(87, "функция is_simple дошла до конца без return");
    return 0;
}

static void f_emit__emit_key(tb_o_Comp * v_c, long long v_dt, tb_o_Node * v_key) {
    f_emit__put(v_c, ({ const char *tb_c652_0 = "&("; long long tb_c652_0n = sizeof("&(") - 1; const char *tb_c652_1 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 93))->q_types, v_dt, sizeof(tb_o_Type *), 93)), 93))->q_key); long long tb_c652_1n = (long long)strlen(tb_c652_1); const char *tb_c652_2 = "){"; long long tb_c652_2n = sizeof("){") - 1; char *tb_c652r = tb_mem(tb_c652_0n + tb_c652_1n + tb_c652_2n + 1), *tb_c652rw = tb_c652r; memcpy(tb_c652rw, tb_c652_0, tb_c652_0n); tb_c652rw += tb_c652_0n; memcpy(tb_c652rw, tb_c652_1, tb_c652_1n); tb_c652rw += tb_c652_1n; memcpy(tb_c652rw, tb_c652_2, tb_c652_2n); tb_c652rw += tb_c652_2n; *tb_c652rw = 0; (const char *)tb_c652r; }));
    f_emit__emit_expr(v_c, v_key);
    f_emit__put(v_c, "}");
}

static void f_emit__emit_as(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_want) {
    if ((({ const char *tb_s653 = f_check__kind_of(v_c, v_want); ((unsigned char)tb_s653[0] == 100 && (unsigned char)tb_s653[1] == 101 && (unsigned char)tb_s653[2] == 99 && tb_s653[3] == 0); }) && ({ const char *tb_s654 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 101))->q_ty); ((unsigned char)tb_s654[0] == 105 && (unsigned char)tb_s654[1] == 110 && (unsigned char)tb_s654[2] == 116 && tb_s654[3] == 0); }))) {
        f_emit__put(v_c, "(double)(");
        f_emit__emit_expr(v_c, v_n);
        f_emit__put(v_c, ")");
        return;
    }
    f_emit__emit_expr(v_c, v_n);
}

static void f_emit__emit_as_text(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 110))->q_ty);
    if (({ const char *tb_s655 = v_k; ((unsigned char)tb_s655[0] == 116 && (unsigned char)tb_s655[1] == 101 && (unsigned char)tb_s655[2] == 120 && (unsigned char)tb_s655[3] == 116 && tb_s655[4] == 0); })) {
        f_emit__emit_q(v_c, v_n);
        return;
    }
    if (({ const char *tb_s656 = v_k; ((unsigned char)tb_s656[0] == 105 && (unsigned char)tb_s656[1] == 110 && (unsigned char)tb_s656[2] == 116 && tb_s656[3] == 0); })) {
        f_emit__put(v_c, "tb_itos(");
    } else if (({ const char *tb_s657 = v_k; ((unsigned char)tb_s657[0] == 100 && (unsigned char)tb_s657[1] == 101 && (unsigned char)tb_s657[2] == 99 && tb_s657[3] == 0); })) {
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
        long long tb_end658 = ((((tb_o_Node *)tb_nn(v_n, 125))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end658; v_i++) {
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 126))->q_kids, v_i, sizeof(tb_o_Node *), 126)), 126))->q_kids, 0LL, sizeof(tb_o_Node *), 126));
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", tb_ps(\" \"), ");
            }
            f_emit__put(v_c, ({ const char *tb_c659_0 = "tb_show_"; long long tb_c659_0n = sizeof("tb_show_") - 1; char tb_c659_1d[24]; long long tb_c659_1n; const char *tb_c659_1 = tb_digits(((tb_o_Node *)tb_nn(v_value, 129))->q_ty, tb_c659_1d, &tb_c659_1n); const char *tb_c659_2 = "("; long long tb_c659_2n = sizeof("(") - 1; char *tb_c659r = tb_mem(tb_c659_0n + tb_c659_1n + tb_c659_2n + 1), *tb_c659rw = tb_c659r; memcpy(tb_c659rw, tb_c659_0, tb_c659_0n); tb_c659rw += tb_c659_0n; memcpy(tb_c659rw, tb_c659_1, tb_c659_1n); tb_c659rw += tb_c659_1n; memcpy(tb_c659rw, tb_c659_2, tb_c659_2n); tb_c659rw += tb_c659_2n; *tb_c659rw = 0; (const char *)tb_c659r; }));
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
        long long tb_end660 = ((((tb_o_Node *)tb_nn(v_n, 138))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end660; v_i++) {
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
        f_emit__put(v_c, ({ const char *tb_c661_0 = ""; long long tb_c661_0n = sizeof("") - 1; char tb_c661_1d[24]; long long tb_c661_1n; const char *tb_c661_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 145))->q_line, tb_c661_1d, &tb_c661_1n); char *tb_c661r = tb_mem(tb_c661_0n + tb_c661_1n + 1), *tb_c661rw = tb_c661r; memcpy(tb_c661rw, tb_c661_0, tb_c661_0n); tb_c661rw += tb_c661_0n; memcpy(tb_c661rw, tb_c661_1, tb_c661_1n); tb_c661rw += tb_c661_1n; *tb_c661rw = 0; (const char *)tb_c661r; }));
    }
    f_emit__put(v_c, ")");
}

static void f_emit__emit_builtin(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_name = ((tb_o_Node *)tb_nn(v_n, 149))->q_name;
    if (({ const char *tb_s662 = v_name; ((unsigned char)tb_s662[0] == 112 && (unsigned char)tb_s662[1] == 114 && (unsigned char)tb_s662[2] == 105 && (unsigned char)tb_s662[3] == 110 && (unsigned char)tb_s662[4] == 116 && tb_s662[5] == 0); })) {
        f_emit__emit_print(v_c, v_n);
        return;
    }
    if (({ const char *tb_s663 = v_name; ((unsigned char)tb_s663[0] == 97 && (unsigned char)tb_s663[1] == 114 && (unsigned char)tb_s663[2] == 103 && (unsigned char)tb_s663[3] == 115 && tb_s663[4] == 0); })) {
        f_emit__put(v_c, "tb_args");
        return;
    }
    if (({ const char *tb_s664 = v_name; ((unsigned char)tb_s664[0] == 105 && (unsigned char)tb_s664[1] == 110 && (unsigned char)tb_s664[2] == 112 && (unsigned char)tb_s664[3] == 117 && (unsigned char)tb_s664[4] == 116 && tb_s664[5] == 0); })) {
        f_emit__put(v_c, "tb_input()");
        return;
    }
    tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 159))->q_kids, 0LL, sizeof(tb_o_Node *), 159)), 159))->q_kids, 0LL, sizeof(tb_o_Node *), 159));
    if (({ const char *tb_s665 = v_name; ((unsigned char)tb_s665[0] == 108 && (unsigned char)tb_s665[1] == 101 && (unsigned char)tb_s665[2] == 110 && tb_s665[3] == 0); })) {
        const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 161))->q_ty);
        if ((({ const char *tb_s666 = v_k; ((unsigned char)tb_s666[0] == 116 && (unsigned char)tb_s666[1] == 101 && (unsigned char)tb_s666[2] == 120 && (unsigned char)tb_s666[3] == 116 && tb_s666[4] == 0); }) && f_emit__is_bld(v_c, v_a))) {
            f_emit__put(v_c, ({ const char *tb_c667_0 = "(tb_rl_"; long long tb_c667_0n = sizeof("(tb_rl_") - 1; const char *tb_c667_1 = ((tb_o_Node *)tb_nn(v_a, 163))->q_name; long long tb_c667_1n = (long long)strlen(tb_c667_1); const char *tb_c667_2 = " >= 0 ? tb_rl_"; long long tb_c667_2n = sizeof(" >= 0 ? tb_rl_") - 1; const char *tb_c667_3 = ((tb_o_Node *)tb_nn(v_a, 163))->q_name; long long tb_c667_3n = (long long)strlen(tb_c667_3); const char *tb_c667_4 = " : tb_tlen(v_"; long long tb_c667_4n = sizeof(" : tb_tlen(v_") - 1; const char *tb_c667_5 = ((tb_o_Node *)tb_nn(v_a, 163))->q_name; long long tb_c667_5n = (long long)strlen(tb_c667_5); const char *tb_c667_6 = "))"; long long tb_c667_6n = sizeof("))") - 1; char *tb_c667r = tb_mem(tb_c667_0n + tb_c667_1n + tb_c667_2n + tb_c667_3n + tb_c667_4n + tb_c667_5n + tb_c667_6n + 1), *tb_c667rw = tb_c667r; memcpy(tb_c667rw, tb_c667_0, tb_c667_0n); tb_c667rw += tb_c667_0n; memcpy(tb_c667rw, tb_c667_1, tb_c667_1n); tb_c667rw += tb_c667_1n; memcpy(tb_c667rw, tb_c667_2, tb_c667_2n); tb_c667rw += tb_c667_2n; memcpy(tb_c667rw, tb_c667_3, tb_c667_3n); tb_c667rw += tb_c667_3n; memcpy(tb_c667rw, tb_c667_4, tb_c667_4n); tb_c667rw += tb_c667_4n; memcpy(tb_c667rw, tb_c667_5, tb_c667_5n); tb_c667rw += tb_c667_5n; memcpy(tb_c667rw, tb_c667_6, tb_c667_6n); tb_c667rw += tb_c667_6n; *tb_c667rw = 0; (const char *)tb_c667r; }));
        } else if (({ const char *tb_s668 = v_k; ((unsigned char)tb_s668[0] == 116 && (unsigned char)tb_s668[1] == 101 && (unsigned char)tb_s668[2] == 120 && (unsigned char)tb_s668[3] == 116 && tb_s668[4] == 0); })) {
            f_emit__put(v_c, "tb_tlen(");
            f_emit__emit_expr(v_c, v_a);
            f_emit__put(v_c, ")");
        } else if (({ const char *tb_s669 = v_k; ((unsigned char)tb_s669[0] == 100 && (unsigned char)tb_s669[1] == 105 && (unsigned char)tb_s669[2] == 99 && (unsigned char)tb_s669[3] == 116 && tb_s669[4] == 0); })) {
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
    if (({ const char *tb_s670 = v_name; ((unsigned char)tb_s670[0] == 115 && (unsigned char)tb_s670[1] == 111 && (unsigned char)tb_s670[2] == 114 && (unsigned char)tb_s670[3] == 116 && tb_s670[4] == 0); })) {
        const char * v_k = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 178))->q_types, ((tb_o_Node *)tb_nn(v_a, 178))->q_ty, sizeof(tb_o_Type *), 178)), 178))->q_elem);
        f_emit__put(v_c, "tb_sort_list(");
        f_emit__emit_expr(v_c, v_a);
        if (({ const char *tb_s671 = v_k; ((unsigned char)tb_s671[0] == 105 && (unsigned char)tb_s671[1] == 110 && (unsigned char)tb_s671[2] == 116 && tb_s671[3] == 0); })) {
            f_emit__put(v_c, ", tb_cmp_int)");
        } else if (({ const char *tb_s672 = v_k; ((unsigned char)tb_s672[0] == 100 && (unsigned char)tb_s672[1] == 101 && (unsigned char)tb_s672[2] == 99 && tb_s672[3] == 0); })) {
            f_emit__put(v_c, ", tb_cmp_dec)");
        } else {
            f_emit__put(v_c, ", tb_cmp_text)");
        }
        return;
    }
    if (({ const char *tb_s673 = v_name; ((unsigned char)tb_s673[0] == 97 && (unsigned char)tb_s673[1] == 98 && (unsigned char)tb_s673[2] == 115 && tb_s673[3] == 0); })) {
        if (({ const char *tb_s674 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 189))->q_ty); ((unsigned char)tb_s674[0] == 105 && (unsigned char)tb_s674[1] == 110 && (unsigned char)tb_s674[2] == 116 && tb_s674[3] == 0); })) {
            f_emit__put(v_c, "tb_absi(");
        } else {
            f_emit__put(v_c, "fabs(");
        }
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ")");
        return;
    }
    if (({ const char *tb_s675 = v_name; ((unsigned char)tb_s675[0] == 115 && (unsigned char)tb_s675[1] == 113 && (unsigned char)tb_s675[2] == 114 && (unsigned char)tb_s675[3] == 116 && tb_s675[4] == 0); })) {
        f_emit__put(v_c, "tb_sqrt((double)(");
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ({ const char *tb_c676_0 = "), "; long long tb_c676_0n = sizeof("), ") - 1; char tb_c676_1d[24]; long long tb_c676_1n; const char *tb_c676_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 199))->q_line, tb_c676_1d, &tb_c676_1n); const char *tb_c676_2 = ")"; long long tb_c676_2n = sizeof(")") - 1; char *tb_c676r = tb_mem(tb_c676_0n + tb_c676_1n + tb_c676_2n + 1), *tb_c676rw = tb_c676r; memcpy(tb_c676rw, tb_c676_0, tb_c676_0n); tb_c676rw += tb_c676_0n; memcpy(tb_c676rw, tb_c676_1, tb_c676_1n); tb_c676rw += tb_c676_1n; memcpy(tb_c676rw, tb_c676_2, tb_c676_2n); tb_c676rw += tb_c676_2n; *tb_c676rw = 0; (const char *)tb_c676r; }));
        return;
    }
    if ((({ const char *tb_s677 = v_name; ((unsigned char)tb_s677[0] == 109 && (unsigned char)tb_s677[1] == 105 && (unsigned char)tb_s677[2] == 110 && tb_s677[3] == 0); }) || ({ const char *tb_s678 = v_name; ((unsigned char)tb_s678[0] == 109 && (unsigned char)tb_s678[1] == 97 && (unsigned char)tb_s678[2] == 120 && tb_s678[3] == 0); }))) {
        _Bool v_ints = ({ const char *tb_s679 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 202))->q_ty); ((unsigned char)tb_s679[0] == 105 && (unsigned char)tb_s679[1] == 110 && (unsigned char)tb_s679[2] == 116 && tb_s679[3] == 0); });
        if (v_ints) {
            f_emit__put(v_c, ({ const char *tb_c680_0 = "tb_"; long long tb_c680_0n = sizeof("tb_") - 1; const char *tb_c680_1 = v_name; long long tb_c680_1n = (long long)strlen(tb_c680_1); const char *tb_c680_2 = "i("; long long tb_c680_2n = sizeof("i(") - 1; char *tb_c680r = tb_mem(tb_c680_0n + tb_c680_1n + tb_c680_2n + 1), *tb_c680rw = tb_c680r; memcpy(tb_c680rw, tb_c680_0, tb_c680_0n); tb_c680rw += tb_c680_0n; memcpy(tb_c680rw, tb_c680_1, tb_c680_1n); tb_c680rw += tb_c680_1n; memcpy(tb_c680rw, tb_c680_2, tb_c680_2n); tb_c680rw += tb_c680_2n; *tb_c680rw = 0; (const char *)tb_c680r; }));
            f_emit__emit_expr(v_c, v_a);
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 207))->q_kids, 1LL, sizeof(tb_o_Node *), 207)), 207))->q_kids, 0LL, sizeof(tb_o_Node *), 207)));
        } else {
            f_emit__put(v_c, ({ const char *tb_c681_0 = "tb_"; long long tb_c681_0n = sizeof("tb_") - 1; const char *tb_c681_1 = v_name; long long tb_c681_1n = (long long)strlen(tb_c681_1); const char *tb_c681_2 = "d("; long long tb_c681_2n = sizeof("d(") - 1; char *tb_c681r = tb_mem(tb_c681_0n + tb_c681_1n + tb_c681_2n + 1), *tb_c681rw = tb_c681r; memcpy(tb_c681rw, tb_c681_0, tb_c681_0n); tb_c681rw += tb_c681_0n; memcpy(tb_c681rw, tb_c681_1, tb_c681_1n); tb_c681rw += tb_c681_1n; memcpy(tb_c681rw, tb_c681_2, tb_c681_2n); tb_c681rw += tb_c681_2n; *tb_c681rw = 0; (const char *)tb_c681r; }));
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
            f_emit__put(v_c, ({ const char *tb_c682_0 = (*(const char * *)tb_at(v_c_names, (v_i + 1LL), sizeof(const char *), 220)); long long tb_c682_0n = (long long)strlen(tb_c682_0); const char *tb_c682_1 = "("; long long tb_c682_1n = sizeof("(") - 1; char *tb_c682r = tb_mem(tb_c682_0n + tb_c682_1n + 1), *tb_c682rw = tb_c682r; memcpy(tb_c682rw, tb_c682_0, tb_c682_0n); tb_c682rw += tb_c682_0n; memcpy(tb_c682rw, tb_c682_1, tb_c682_1n); tb_c682rw += tb_c682_1n; *tb_c682rw = 0; (const char *)tb_c682r; }));
            f_emit__emit_args(v_c, v_n, ({ const char *tb_s683 = (*(const char * *)tb_at(v_c_names, (v_i + 2LL), sizeof(const char *), 221)); ((unsigned char)tb_s683[0] == 49 && tb_s683[1] == 0); }));
            return;
        }
        { long long tb_v684 = 3LL; long long *tb_p684 = &(v_i); *tb_p684 += tb_v684; }
    }
    f_emit__put(v_c, ({ const char *tb_c685_0 = "/* неизвестная встроенная "; long long tb_c685_0n = sizeof("/* неизвестная встроенная ") - 1; const char *tb_c685_1 = v_name; long long tb_c685_1n = (long long)strlen(tb_c685_1); const char *tb_c685_2 = " */0"; long long tb_c685_2n = sizeof(" */0") - 1; char *tb_c685r = tb_mem(tb_c685_0n + tb_c685_1n + tb_c685_2n + 1), *tb_c685rw = tb_c685r; memcpy(tb_c685rw, tb_c685_0, tb_c685_0n); tb_c685rw += tb_c685_0n; memcpy(tb_c685rw, tb_c685_1, tb_c685_1n); tb_c685rw += tb_c685_1n; memcpy(tb_c685rw, tb_c685_2, tb_c685_2n); tb_c685rw += tb_c685_2n; *tb_c685rw = 0; (const char *)tb_c685r; }));
}

static void f_emit__emit_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if ((!((tb_o_Node *)tb_nn(v_n, 227))->q_has_ref)) {
        f_emit__emit_builtin(v_c, v_n);
        return;
    }
    if (({ const char *tb_s686 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 230))->q_ref, 230))->q_kind; ((unsigned char)tb_s686[0] == 111 && (unsigned char)tb_s686[1] == 98 && (unsigned char)tb_s686[2] == 106 && tb_s686[3] == 0); })) {
        tb_o_Node * v_o = ((tb_o_Node *)tb_nn(v_n, 231))->q_ref;
        f_emit__put(v_c, ({ const char *tb_c687_0 = "tb_new_"; long long tb_c687_0n = sizeof("tb_new_") - 1; const char *tb_c687_1 = ((tb_o_Node *)tb_nn(v_o, 232))->q_name; long long tb_c687_1n = (long long)strlen(tb_c687_1); const char *tb_c687_2 = "("; long long tb_c687_2n = sizeof("(") - 1; char *tb_c687r = tb_mem(tb_c687_0n + tb_c687_1n + tb_c687_2n + 1), *tb_c687rw = tb_c687r; memcpy(tb_c687rw, tb_c687_0, tb_c687_0n); tb_c687rw += tb_c687_0n; memcpy(tb_c687rw, tb_c687_1, tb_c687_1n); tb_c687rw += tb_c687_1n; memcpy(tb_c687rw, tb_c687_2, tb_c687_2n); tb_c687rw += tb_c687_2n; *tb_c687rw = 0; (const char *)tb_c687r; }));
        {
            long long tb_end688 = ((((tb_o_Node *)tb_nn(v_o, 233))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end688; v_i++) {
                if ((v_i > 0LL)) {
                    f_emit__put(v_c, ", ");
                }
                if (({ const char *tb_s689 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 236))->q_kids, v_i, sizeof(tb_o_Node *), 236)), 236))->q_kind; ((unsigned char)tb_s689[0] == 109 && (unsigned char)tb_s689[1] == 105 && (unsigned char)tb_s689[2] == 115 && (unsigned char)tb_s689[3] == 115 && (unsigned char)tb_s689[4] == 105 && (unsigned char)tb_s689[5] == 110 && (unsigned char)tb_s689[6] == 103 && tb_s689[7] == 0); })) {
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
    f_emit__put(v_c, ({ const char *tb_c690_0 = f_emit__fname(((tb_o_Node *)tb_nn(v_n, 243))->q_ref); long long tb_c690_0n = (long long)strlen(tb_c690_0); const char *tb_c690_1 = "("; long long tb_c690_1n = sizeof("(") - 1; char *tb_c690r = tb_mem(tb_c690_0n + tb_c690_1n + 1), *tb_c690rw = tb_c690r; memcpy(tb_c690rw, tb_c690_0, tb_c690_0n); tb_c690rw += tb_c690_0n; memcpy(tb_c690rw, tb_c690_1, tb_c690_1n); tb_c690rw += tb_c690_1n; *tb_c690rw = 0; (const char *)tb_c690r; }));
    {
        long long tb_end691 = ((((tb_o_Node *)tb_nn(v_n, 244))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end691; v_i++) {
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
    if (({ const char *tb_s692 = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn(v_t, 260))->q_key); ((unsigned char)tb_s692[0] == 116 && (unsigned char)tb_s692[1] == 101 && (unsigned char)tb_s692[2] == 120 && (unsigned char)tb_s692[3] == 116 && tb_s692[4] == 0); })) {
        v_is_text = "1";
    }
    f_emit__put(v_c, ({ const char *tb_c693_0 = "tb_dict_from("; long long tb_c693_0n = sizeof("tb_dict_from(") - 1; const char *tb_c693_1 = v_is_text; long long tb_c693_1n = (long long)strlen(tb_c693_1); const char *tb_c693_2 = ", sizeof("; long long tb_c693_2n = sizeof(", sizeof(") - 1; const char *tb_c693_3 = v_vt; long long tb_c693_3n = (long long)strlen(tb_c693_3); const char *tb_c693_4 = "), "; long long tb_c693_4n = sizeof("), ") - 1; char tb_c693_5d[24]; long long tb_c693_5n; const char *tb_c693_5 = tb_digits(tb_div((((tb_o_Node *)tb_nn(v_n, 262))->q_kids)->len, 2LL, 262), tb_c693_5d, &tb_c693_5n); const char *tb_c693_6 = ", ("; long long tb_c693_6n = sizeof(", (") - 1; const char *tb_c693_7 = v_kt; long long tb_c693_7n = (long long)strlen(tb_c693_7); const char *tb_c693_8 = "[]){"; long long tb_c693_8n = sizeof("[]){") - 1; char *tb_c693r = tb_mem(tb_c693_0n + tb_c693_1n + tb_c693_2n + tb_c693_3n + tb_c693_4n + tb_c693_5n + tb_c693_6n + tb_c693_7n + tb_c693_8n + 1), *tb_c693rw = tb_c693r; memcpy(tb_c693rw, tb_c693_0, tb_c693_0n); tb_c693rw += tb_c693_0n; memcpy(tb_c693rw, tb_c693_1, tb_c693_1n); tb_c693rw += tb_c693_1n; memcpy(tb_c693rw, tb_c693_2, tb_c693_2n); tb_c693rw += tb_c693_2n; memcpy(tb_c693rw, tb_c693_3, tb_c693_3n); tb_c693rw += tb_c693_3n; memcpy(tb_c693rw, tb_c693_4, tb_c693_4n); tb_c693rw += tb_c693_4n; memcpy(tb_c693rw, tb_c693_5, tb_c693_5n); tb_c693rw += tb_c693_5n; memcpy(tb_c693rw, tb_c693_6, tb_c693_6n); tb_c693rw += tb_c693_6n; memcpy(tb_c693rw, tb_c693_7, tb_c693_7n); tb_c693rw += tb_c693_7n; memcpy(tb_c693rw, tb_c693_8, tb_c693_8n); tb_c693rw += tb_c693_8n; *tb_c693rw = 0; (const char *)tb_c693r; }));
    long long v_i = 0LL;
    while ((v_i < (((tb_o_Node *)tb_nn(v_n, 264))->q_kids)->len)) {
        if ((v_i > 0LL)) {
            f_emit__put(v_c, ", ");
        }
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 267))->q_kids, v_i, sizeof(tb_o_Node *), 267)));
        { long long tb_v694 = 2LL; long long *tb_p694 = &(v_i); *tb_p694 += tb_v694; }
    }
    f_emit__put(v_c, ({ const char *tb_c695_0 = "}, ("; long long tb_c695_0n = sizeof("}, (") - 1; const char *tb_c695_1 = v_vt; long long tb_c695_1n = (long long)strlen(tb_c695_1); const char *tb_c695_2 = "[]){"; long long tb_c695_2n = sizeof("[]){") - 1; char *tb_c695r = tb_mem(tb_c695_0n + tb_c695_1n + tb_c695_2n + 1), *tb_c695rw = tb_c695r; memcpy(tb_c695rw, tb_c695_0, tb_c695_0n); tb_c695rw += tb_c695_0n; memcpy(tb_c695rw, tb_c695_1, tb_c695_1n); tb_c695rw += tb_c695_1n; memcpy(tb_c695rw, tb_c695_2, tb_c695_2n); tb_c695rw += tb_c695_2n; *tb_c695rw = 0; (const char *)tb_c695r; }));
    v_i = 1LL;
    while ((v_i < (((tb_o_Node *)tb_nn(v_n, 271))->q_kids)->len)) {
        if ((v_i > 1LL)) {
            f_emit__put(v_c, ", ");
        }
        f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 274))->q_kids, v_i, sizeof(tb_o_Node *), 274)), ((tb_o_Type *)tb_nn(v_t, 274))->q_elem);
        { long long tb_v696 = 2LL; long long *tb_p696 = &(v_i); *tb_p696 += tb_v696; }
    }
    f_emit__put(v_c, "})");
}

static void f_emit__emit_method(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_base = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 280))->q_kids, 0LL, sizeof(tb_o_Node *), 280));
    if (({ const char *tb_s697 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_base, 281))->q_ty); ((unsigned char)tb_s697[0] == 100 && (unsigned char)tb_s697[1] == 105 && (unsigned char)tb_s697[2] == 99 && (unsigned char)tb_s697[3] == 116 && tb_s697[4] == 0); })) {
        if ((({ const char *tb_s698 = ((tb_o_Node *)tb_nn(v_n, 282))->q_name; ((unsigned char)tb_s698[0] == 118 && (unsigned char)tb_s698[1] == 97 && (unsigned char)tb_s698[2] == 108 && (unsigned char)tb_s698[3] == 117 && (unsigned char)tb_s698[4] == 101 && (unsigned char)tb_s698[5] == 115 && tb_s698[6] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_n, 282))->q_ty))) {
            f_emit__put(v_c, ({ const char *tb_c699_0 = "tb_colsfrom_"; long long tb_c699_0n = sizeof("tb_colsfrom_") - 1; const char *tb_c699_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn(v_n, 283))->q_ty); long long tb_c699_1n = (long long)strlen(tb_c699_1); const char *tb_c699_2 = "(("; long long tb_c699_2n = sizeof("((") - 1; char *tb_c699r = tb_mem(tb_c699_0n + tb_c699_1n + tb_c699_2n + 1), *tb_c699rw = tb_c699r; memcpy(tb_c699rw, tb_c699_0, tb_c699_0n); tb_c699rw += tb_c699_0n; memcpy(tb_c699rw, tb_c699_1, tb_c699_1n); tb_c699rw += tb_c699_1n; memcpy(tb_c699rw, tb_c699_2, tb_c699_2n); tb_c699rw += tb_c699_2n; *tb_c699rw = 0; (const char *)tb_c699r; }));
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, ({ const char *tb_c700_0 = ")->vals->len, ("; long long tb_c700_0n = sizeof(")->vals->len, (") - 1; const char *tb_c700_1 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 285))->q_types, ((tb_o_Node *)tb_nn(v_n, 285))->q_ty, sizeof(tb_o_Type *), 285)), 285))->q_elem); long long tb_c700_1n = (long long)strlen(tb_c700_1); const char *tb_c700_2 = " *)("; long long tb_c700_2n = sizeof(" *)(") - 1; char *tb_c700r = tb_mem(tb_c700_0n + tb_c700_1n + tb_c700_2n + 1), *tb_c700rw = tb_c700r; memcpy(tb_c700rw, tb_c700_0, tb_c700_0n); tb_c700rw += tb_c700_0n; memcpy(tb_c700rw, tb_c700_1, tb_c700_1n); tb_c700rw += tb_c700_1n; memcpy(tb_c700rw, tb_c700_2, tb_c700_2n); tb_c700rw += tb_c700_2n; *tb_c700rw = 0; (const char *)tb_c700r; }));
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, ")->vals->data)");
            return;
        }
        if ((({ const char *tb_s701 = ((tb_o_Node *)tb_nn(v_n, 289))->q_name; ((unsigned char)tb_s701[0] == 118 && (unsigned char)tb_s701[1] == 97 && (unsigned char)tb_s701[2] == 108 && (unsigned char)tb_s701[3] == 117 && (unsigned char)tb_s701[4] == 101 && (unsigned char)tb_s701[5] == 115 && tb_s701[6] == 0); }) && f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_n, 289))->q_ty))) {
            f_emit__put(v_c, "tb_bits_of_bytes((");
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, ")->vals)");
            return;
        }
        if ((({ const char *tb_s702 = ((tb_o_Node *)tb_nn(v_n, 294))->q_name; ((unsigned char)tb_s702[0] == 107 && (unsigned char)tb_s702[1] == 101 && (unsigned char)tb_s702[2] == 121 && (unsigned char)tb_s702[3] == 115 && tb_s702[4] == 0); }) || ({ const char *tb_s703 = ((tb_o_Node *)tb_nn(v_n, 294))->q_name; ((unsigned char)tb_s703[0] == 118 && (unsigned char)tb_s703[1] == 97 && (unsigned char)tb_s703[2] == 108 && (unsigned char)tb_s703[3] == 117 && (unsigned char)tb_s703[4] == 101 && (unsigned char)tb_s703[5] == 115 && tb_s703[6] == 0); }))) {
            f_emit__put(v_c, "tb_copy((");
            f_emit__emit_expr(v_c, v_base);
            if (({ const char *tb_s704 = ((tb_o_Node *)tb_nn(v_n, 297))->q_name; ((unsigned char)tb_s704[0] == 107 && (unsigned char)tb_s704[1] == 101 && (unsigned char)tb_s704[2] == 121 && (unsigned char)tb_s704[3] == 115 && tb_s704[4] == 0); })) {
                f_emit__put(v_c, ")->keys)");
            } else {
                f_emit__put(v_c, ")->vals)");
            }
            return;
        }
        if (({ const char *tb_s705 = ((tb_o_Node *)tb_nn(v_n, 302))->q_name; ((unsigned char)tb_s705[0] == 104 && (unsigned char)tb_s705[1] == 97 && (unsigned char)tb_s705[2] == 115 && tb_s705[3] == 0); })) {
            f_emit__put(v_c, "tb_dhas(");
        } else {
            f_emit__put(v_c, "tb_dremove(");
        }
        f_emit__emit_expr(v_c, v_base);
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, ((tb_o_Node *)tb_nn(v_base, 308))->q_ty, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 308))->q_kids, 1LL, sizeof(tb_o_Node *), 308)), 308))->q_kids, 0LL, sizeof(tb_o_Node *), 308)));
        f_emit__put(v_c, ")");
        return;
    }
    if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_base, 311))->q_ty)) {
        f_emit__put(v_c, "tb_bpop(");
        f_emit__emit_expr(v_c, v_base);
        f_emit__put(v_c, ({ const char *tb_c706_0 = ", "; long long tb_c706_0n = sizeof(", ") - 1; char tb_c706_1d[24]; long long tb_c706_1n; const char *tb_c706_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 314))->q_line, tb_c706_1d, &tb_c706_1n); const char *tb_c706_2 = ")"; long long tb_c706_2n = sizeof(")") - 1; char *tb_c706r = tb_mem(tb_c706_0n + tb_c706_1n + tb_c706_2n + 1), *tb_c706rw = tb_c706r; memcpy(tb_c706rw, tb_c706_0, tb_c706_0n); tb_c706rw += tb_c706_0n; memcpy(tb_c706rw, tb_c706_1, tb_c706_1n); tb_c706rw += tb_c706_1n; memcpy(tb_c706rw, tb_c706_2, tb_c706_2n); tb_c706rw += tb_c706_2n; *tb_c706rw = 0; (const char *)tb_c706r; }));
        return;
    }
    f_emit__put(v_c, ({ const char *tb_c707_0 = "(*("; long long tb_c707_0n = sizeof("(*(") - 1; const char *tb_c707_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 316))->q_ty); long long tb_c707_1n = (long long)strlen(tb_c707_1); const char *tb_c707_2 = " *)tb_pop("; long long tb_c707_2n = sizeof(" *)tb_pop(") - 1; char *tb_c707r = tb_mem(tb_c707_0n + tb_c707_1n + tb_c707_2n + 1), *tb_c707rw = tb_c707r; memcpy(tb_c707rw, tb_c707_0, tb_c707_0n); tb_c707rw += tb_c707_0n; memcpy(tb_c707rw, tb_c707_1, tb_c707_1n); tb_c707rw += tb_c707_1n; memcpy(tb_c707rw, tb_c707_2, tb_c707_2n); tb_c707rw += tb_c707_2n; *tb_c707rw = 0; (const char *)tb_c707r; }));
    f_emit__emit_expr(v_c, v_base);
    f_emit__put(v_c, ({ const char *tb_c708_0 = ", "; long long tb_c708_0n = sizeof(", ") - 1; char tb_c708_1d[24]; long long tb_c708_1n; const char *tb_c708_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 318))->q_line, tb_c708_1d, &tb_c708_1n); const char *tb_c708_2 = "))"; long long tb_c708_2n = sizeof("))") - 1; char *tb_c708r = tb_mem(tb_c708_0n + tb_c708_1n + tb_c708_2n + 1), *tb_c708rw = tb_c708r; memcpy(tb_c708rw, tb_c708_0, tb_c708_0n); tb_c708rw += tb_c708_0n; memcpy(tb_c708rw, tb_c708_1, tb_c708_1n); tb_c708rw += tb_c708_1n; memcpy(tb_c708rw, tb_c708_2, tb_c708_2n); tb_c708rw += tb_c708_2n; *tb_c708rw = 0; (const char *)tb_c708r; }));
}

static void f_emit__emit_list(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 321))->q_types, ((tb_o_Node *)tb_nn(v_n, 321))->q_ty, sizeof(tb_o_Type *), 321)), 321))->q_elem;
    const char * v_elem = f_emit__c_type(v_c, v_elem_ty);
    if (f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_n, 323))->q_ty)) {
        f_emit__put(v_c, ({ const char *tb_c709_0 = "tb_colsfrom_"; long long tb_c709_0n = sizeof("tb_colsfrom_") - 1; const char *tb_c709_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn(v_n, 324))->q_ty); long long tb_c709_1n = (long long)strlen(tb_c709_1); const char *tb_c709_2 = "("; long long tb_c709_2n = sizeof("(") - 1; char tb_c709_3d[24]; long long tb_c709_3n; const char *tb_c709_3 = tb_digits((((tb_o_Node *)tb_nn(v_n, 324))->q_kids)->len, tb_c709_3d, &tb_c709_3n); const char *tb_c709_4 = ", ("; long long tb_c709_4n = sizeof(", (") - 1; const char *tb_c709_5 = v_elem; long long tb_c709_5n = (long long)strlen(tb_c709_5); const char *tb_c709_6 = "[]){"; long long tb_c709_6n = sizeof("[]){") - 1; char *tb_c709r = tb_mem(tb_c709_0n + tb_c709_1n + tb_c709_2n + tb_c709_3n + tb_c709_4n + tb_c709_5n + tb_c709_6n + 1), *tb_c709rw = tb_c709r; memcpy(tb_c709rw, tb_c709_0, tb_c709_0n); tb_c709rw += tb_c709_0n; memcpy(tb_c709rw, tb_c709_1, tb_c709_1n); tb_c709rw += tb_c709_1n; memcpy(tb_c709rw, tb_c709_2, tb_c709_2n); tb_c709rw += tb_c709_2n; memcpy(tb_c709rw, tb_c709_3, tb_c709_3n); tb_c709rw += tb_c709_3n; memcpy(tb_c709rw, tb_c709_4, tb_c709_4n); tb_c709rw += tb_c709_4n; memcpy(tb_c709rw, tb_c709_5, tb_c709_5n); tb_c709rw += tb_c709_5n; memcpy(tb_c709rw, tb_c709_6, tb_c709_6n); tb_c709rw += tb_c709_6n; *tb_c709rw = 0; (const char *)tb_c709r; }));
    } else if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_n, 325))->q_ty)) {
        f_emit__put(v_c, ({ const char *tb_c710_0 = "tb_bits_from("; long long tb_c710_0n = sizeof("tb_bits_from(") - 1; char tb_c710_1d[24]; long long tb_c710_1n; const char *tb_c710_1 = tb_digits((((tb_o_Node *)tb_nn(v_n, 326))->q_kids)->len, tb_c710_1d, &tb_c710_1n); const char *tb_c710_2 = ", (unsigned char[]){"; long long tb_c710_2n = sizeof(", (unsigned char[]){") - 1; char *tb_c710r = tb_mem(tb_c710_0n + tb_c710_1n + tb_c710_2n + 1), *tb_c710rw = tb_c710r; memcpy(tb_c710rw, tb_c710_0, tb_c710_0n); tb_c710rw += tb_c710_0n; memcpy(tb_c710rw, tb_c710_1, tb_c710_1n); tb_c710rw += tb_c710_1n; memcpy(tb_c710rw, tb_c710_2, tb_c710_2n); tb_c710rw += tb_c710_2n; *tb_c710rw = 0; (const char *)tb_c710r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c711_0 = "tb_list_from("; long long tb_c711_0n = sizeof("tb_list_from(") - 1; char tb_c711_1d[24]; long long tb_c711_1n; const char *tb_c711_1 = tb_digits((((tb_o_Node *)tb_nn(v_n, 328))->q_kids)->len, tb_c711_1d, &tb_c711_1n); const char *tb_c711_2 = ", sizeof("; long long tb_c711_2n = sizeof(", sizeof(") - 1; const char *tb_c711_3 = v_elem; long long tb_c711_3n = (long long)strlen(tb_c711_3); const char *tb_c711_4 = "), ("; long long tb_c711_4n = sizeof("), (") - 1; const char *tb_c711_5 = v_elem; long long tb_c711_5n = (long long)strlen(tb_c711_5); const char *tb_c711_6 = "[]){"; long long tb_c711_6n = sizeof("[]){") - 1; char *tb_c711r = tb_mem(tb_c711_0n + tb_c711_1n + tb_c711_2n + tb_c711_3n + tb_c711_4n + tb_c711_5n + tb_c711_6n + 1), *tb_c711rw = tb_c711r; memcpy(tb_c711rw, tb_c711_0, tb_c711_0n); tb_c711rw += tb_c711_0n; memcpy(tb_c711rw, tb_c711_1, tb_c711_1n); tb_c711rw += tb_c711_1n; memcpy(tb_c711rw, tb_c711_2, tb_c711_2n); tb_c711rw += tb_c711_2n; memcpy(tb_c711rw, tb_c711_3, tb_c711_3n); tb_c711rw += tb_c711_3n; memcpy(tb_c711rw, tb_c711_4, tb_c711_4n); tb_c711rw += tb_c711_4n; memcpy(tb_c711rw, tb_c711_5, tb_c711_5n); tb_c711rw += tb_c711_5n; memcpy(tb_c711rw, tb_c711_6, tb_c711_6n); tb_c711rw += tb_c711_6n; *tb_c711rw = 0; (const char *)tb_c711r; }));
    }
    {
        long long tb_end712 = ((((tb_o_Node *)tb_nn(v_n, 329))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end712; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 332))->q_kids, v_i, sizeof(tb_o_Node *), 332)), v_elem_ty);
        }
    }
    f_emit__put(v_c, "})");
}

static void f_emit__concat_parts(tb_o_Comp * v_c, tb_o_Node * v_n, tb_list * v_parts) {
    if (((({ const char *tb_s713 = ((tb_o_Node *)tb_nn(v_n, 337))->q_kind; ((unsigned char)tb_s713[0] == 98 && (unsigned char)tb_s713[1] == 105 && (unsigned char)tb_s713[2] == 110 && (unsigned char)tb_s713[3] == 111 && (unsigned char)tb_s713[4] == 112 && tb_s713[5] == 0); }) && ({ const char *tb_s714 = ((tb_o_Node *)tb_nn(v_n, 337))->q_value; ((unsigned char)tb_s714[0] == 43 && tb_s714[1] == 0); })) && ({ const char *tb_s715 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 337))->q_ty); ((unsigned char)tb_s715[0] == 116 && (unsigned char)tb_s715[1] == 101 && (unsigned char)tb_s715[2] == 120 && (unsigned char)tb_s715[3] == 116 && tb_s715[4] == 0); }))) {
        f_emit__concat_parts(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 338))->q_kids, 0LL, sizeof(tb_o_Node *), 338)), v_parts);
        f_emit__concat_parts(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 339))->q_kids, 1LL, sizeof(tb_o_Node *), 339)), v_parts);
    } else {
        { tb_o_Node * tb_v716 = v_n; *(tb_o_Node * *)tb_grow(v_parts) = tb_v716; }
    }
}

static void f_emit__emit_concat(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long tb_rl_total = -1, tb_rc_total = 0; (void)tb_rl_total; (void)tb_rc_total;
    tb_list * v_parts = tb_list_new(sizeof(tb_o_Node *));
    f_emit__concat_parts(v_c, v_n, v_parts);
    { long long tb_v717 = 1LL; long long *tb_p717 = &(((tb_o_Comp *)tb_nn(v_c, 349))->q_label); *tb_p717 += tb_v717; }
    const char * v_id = ({ const char *tb_c718_0 = ""; long long tb_c718_0n = sizeof("") - 1; char tb_c718_1d[24]; long long tb_c718_1n; const char *tb_c718_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 350))->q_label, tb_c718_1d, &tb_c718_1n); char *tb_c718r = tb_mem(tb_c718_0n + tb_c718_1n + 1), *tb_c718rw = tb_c718r; memcpy(tb_c718rw, tb_c718_0, tb_c718_0n); tb_c718rw += tb_c718_0n; memcpy(tb_c718rw, tb_c718_1, tb_c718_1n); tb_c718rw += tb_c718_1n; *tb_c718rw = 0; (const char *)tb_c718r; });
    f_emit__put(v_c, "({ ");
    const char * v_total = ""; tb_rl_total = -1;
    {
        long long tb_end719 = ((v_parts)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end719; v_i++) {
            tb_o_Node * v_part = (*(tb_o_Node * *)tb_at(v_parts, v_i, sizeof(tb_o_Node *), 354));
            const char * v_v = ({ const char *tb_c720_0 = "tb_c"; long long tb_c720_0n = sizeof("tb_c") - 1; const char *tb_c720_1 = v_id; long long tb_c720_1n = (long long)strlen(tb_c720_1); const char *tb_c720_2 = "_"; long long tb_c720_2n = sizeof("_") - 1; char tb_c720_3d[24]; long long tb_c720_3n; const char *tb_c720_3 = tb_digits(v_i, tb_c720_3d, &tb_c720_3n); char *tb_c720r = tb_mem(tb_c720_0n + tb_c720_1n + tb_c720_2n + tb_c720_3n + 1), *tb_c720rw = tb_c720r; memcpy(tb_c720rw, tb_c720_0, tb_c720_0n); tb_c720rw += tb_c720_0n; memcpy(tb_c720rw, tb_c720_1, tb_c720_1n); tb_c720rw += tb_c720_1n; memcpy(tb_c720rw, tb_c720_2, tb_c720_2n); tb_c720rw += tb_c720_2n; memcpy(tb_c720rw, tb_c720_3, tb_c720_3n); tb_c720rw += tb_c720_3n; *tb_c720rw = 0; (const char *)tb_c720r; });
            const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_part, 356))->q_ty);
            if (({ const char *tb_s721 = v_k; ((unsigned char)tb_s721[0] == 105 && (unsigned char)tb_s721[1] == 110 && (unsigned char)tb_s721[2] == 116 && tb_s721[3] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c722_0 = "char "; long long tb_c722_0n = sizeof("char ") - 1; const char *tb_c722_1 = v_v; long long tb_c722_1n = (long long)strlen(tb_c722_1); const char *tb_c722_2 = "d[24]; long long "; long long tb_c722_2n = sizeof("d[24]; long long ") - 1; const char *tb_c722_3 = v_v; long long tb_c722_3n = (long long)strlen(tb_c722_3); const char *tb_c722_4 = "n; const char *"; long long tb_c722_4n = sizeof("n; const char *") - 1; const char *tb_c722_5 = v_v; long long tb_c722_5n = (long long)strlen(tb_c722_5); const char *tb_c722_6 = " = tb_digits("; long long tb_c722_6n = sizeof(" = tb_digits(") - 1; char *tb_c722r = tb_mem(tb_c722_0n + tb_c722_1n + tb_c722_2n + tb_c722_3n + tb_c722_4n + tb_c722_5n + tb_c722_6n + 1), *tb_c722rw = tb_c722r; memcpy(tb_c722rw, tb_c722_0, tb_c722_0n); tb_c722rw += tb_c722_0n; memcpy(tb_c722rw, tb_c722_1, tb_c722_1n); tb_c722rw += tb_c722_1n; memcpy(tb_c722rw, tb_c722_2, tb_c722_2n); tb_c722rw += tb_c722_2n; memcpy(tb_c722rw, tb_c722_3, tb_c722_3n); tb_c722rw += tb_c722_3n; memcpy(tb_c722rw, tb_c722_4, tb_c722_4n); tb_c722rw += tb_c722_4n; memcpy(tb_c722rw, tb_c722_5, tb_c722_5n); tb_c722rw += tb_c722_5n; memcpy(tb_c722rw, tb_c722_6, tb_c722_6n); tb_c722rw += tb_c722_6n; *tb_c722rw = 0; (const char *)tb_c722r; }));
                f_emit__emit_expr(v_c, v_part);
                f_emit__put(v_c, ({ const char *tb_c723_0 = ", "; long long tb_c723_0n = sizeof(", ") - 1; const char *tb_c723_1 = v_v; long long tb_c723_1n = (long long)strlen(tb_c723_1); const char *tb_c723_2 = "d, &"; long long tb_c723_2n = sizeof("d, &") - 1; const char *tb_c723_3 = v_v; long long tb_c723_3n = (long long)strlen(tb_c723_3); const char *tb_c723_4 = "n); "; long long tb_c723_4n = sizeof("n); ") - 1; char *tb_c723r = tb_mem(tb_c723_0n + tb_c723_1n + tb_c723_2n + tb_c723_3n + tb_c723_4n + 1), *tb_c723rw = tb_c723r; memcpy(tb_c723rw, tb_c723_0, tb_c723_0n); tb_c723rw += tb_c723_0n; memcpy(tb_c723rw, tb_c723_1, tb_c723_1n); tb_c723rw += tb_c723_1n; memcpy(tb_c723rw, tb_c723_2, tb_c723_2n); tb_c723rw += tb_c723_2n; memcpy(tb_c723rw, tb_c723_3, tb_c723_3n); tb_c723rw += tb_c723_3n; memcpy(tb_c723rw, tb_c723_4, tb_c723_4n); tb_c723rw += tb_c723_4n; *tb_c723rw = 0; (const char *)tb_c723r; }));
            } else if (({ const char *tb_s724 = ((tb_o_Node *)tb_nn(v_part, 361))->q_kind; ((unsigned char)tb_s724[0] == 115 && (unsigned char)tb_s724[1] == 116 && (unsigned char)tb_s724[2] == 114 && (unsigned char)tb_s724[3] == 105 && (unsigned char)tb_s724[4] == 110 && (unsigned char)tb_s724[5] == 103 && tb_s724[6] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c725_0 = "const char *"; long long tb_c725_0n = sizeof("const char *") - 1; const char *tb_c725_1 = v_v; long long tb_c725_1n = (long long)strlen(tb_c725_1); const char *tb_c725_2 = " = "; long long tb_c725_2n = sizeof(" = ") - 1; const char *tb_c725_3 = ((tb_o_Node *)tb_nn(v_part, 362))->q_value; long long tb_c725_3n = (long long)strlen(tb_c725_3); const char *tb_c725_4 = "; long long "; long long tb_c725_4n = sizeof("; long long ") - 1; const char *tb_c725_5 = v_v; long long tb_c725_5n = (long long)strlen(tb_c725_5); const char *tb_c725_6 = "n = sizeof("; long long tb_c725_6n = sizeof("n = sizeof(") - 1; const char *tb_c725_7 = ((tb_o_Node *)tb_nn(v_part, 362))->q_value; long long tb_c725_7n = (long long)strlen(tb_c725_7); const char *tb_c725_8 = ") - 1; "; long long tb_c725_8n = sizeof(") - 1; ") - 1; char *tb_c725r = tb_mem(tb_c725_0n + tb_c725_1n + tb_c725_2n + tb_c725_3n + tb_c725_4n + tb_c725_5n + tb_c725_6n + tb_c725_7n + tb_c725_8n + 1), *tb_c725rw = tb_c725r; memcpy(tb_c725rw, tb_c725_0, tb_c725_0n); tb_c725rw += tb_c725_0n; memcpy(tb_c725rw, tb_c725_1, tb_c725_1n); tb_c725rw += tb_c725_1n; memcpy(tb_c725rw, tb_c725_2, tb_c725_2n); tb_c725rw += tb_c725_2n; memcpy(tb_c725rw, tb_c725_3, tb_c725_3n); tb_c725rw += tb_c725_3n; memcpy(tb_c725rw, tb_c725_4, tb_c725_4n); tb_c725rw += tb_c725_4n; memcpy(tb_c725rw, tb_c725_5, tb_c725_5n); tb_c725rw += tb_c725_5n; memcpy(tb_c725rw, tb_c725_6, tb_c725_6n); tb_c725rw += tb_c725_6n; memcpy(tb_c725rw, tb_c725_7, tb_c725_7n); tb_c725rw += tb_c725_7n; memcpy(tb_c725rw, tb_c725_8, tb_c725_8n); tb_c725rw += tb_c725_8n; *tb_c725rw = 0; (const char *)tb_c725r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c726_0 = "const char *"; long long tb_c726_0n = sizeof("const char *") - 1; const char *tb_c726_1 = v_v; long long tb_c726_1n = (long long)strlen(tb_c726_1); const char *tb_c726_2 = " = "; long long tb_c726_2n = sizeof(" = ") - 1; char *tb_c726r = tb_mem(tb_c726_0n + tb_c726_1n + tb_c726_2n + 1), *tb_c726rw = tb_c726r; memcpy(tb_c726rw, tb_c726_0, tb_c726_0n); tb_c726rw += tb_c726_0n; memcpy(tb_c726rw, tb_c726_1, tb_c726_1n); tb_c726rw += tb_c726_1n; memcpy(tb_c726rw, tb_c726_2, tb_c726_2n); tb_c726rw += tb_c726_2n; *tb_c726rw = 0; (const char *)tb_c726r; }));
                f_emit__emit_as_text(v_c, v_part);
                f_emit__put(v_c, ({ const char *tb_c727_0 = "; long long "; long long tb_c727_0n = sizeof("; long long ") - 1; const char *tb_c727_1 = v_v; long long tb_c727_1n = (long long)strlen(tb_c727_1); const char *tb_c727_2 = "n = (long long)strlen("; long long tb_c727_2n = sizeof("n = (long long)strlen(") - 1; const char *tb_c727_3 = v_v; long long tb_c727_3n = (long long)strlen(tb_c727_3); const char *tb_c727_4 = "); "; long long tb_c727_4n = sizeof("); ") - 1; char *tb_c727r = tb_mem(tb_c727_0n + tb_c727_1n + tb_c727_2n + tb_c727_3n + tb_c727_4n + 1), *tb_c727rw = tb_c727r; memcpy(tb_c727rw, tb_c727_0, tb_c727_0n); tb_c727rw += tb_c727_0n; memcpy(tb_c727rw, tb_c727_1, tb_c727_1n); tb_c727rw += tb_c727_1n; memcpy(tb_c727rw, tb_c727_2, tb_c727_2n); tb_c727rw += tb_c727_2n; memcpy(tb_c727rw, tb_c727_3, tb_c727_3n); tb_c727rw += tb_c727_3n; memcpy(tb_c727rw, tb_c727_4, tb_c727_4n); tb_c727rw += tb_c727_4n; *tb_c727rw = 0; (const char *)tb_c727r; }));
            }
            if ((v_i > 0LL)) {
                tb_app_n(&v_total, &tb_rl_total, &tb_rc_total, " + ", sizeof(" + ") - 1); 
            }
            tb_app(&v_total, &tb_rl_total, &tb_rc_total, v_v); tb_app_n(&v_total, &tb_rl_total, &tb_rc_total, "n", sizeof("n") - 1); 
        }
    }
    const char * v_r = ({ const char *tb_c728_0 = "tb_c"; long long tb_c728_0n = sizeof("tb_c") - 1; const char *tb_c728_1 = v_id; long long tb_c728_1n = (long long)strlen(tb_c728_1); const char *tb_c728_2 = "r"; long long tb_c728_2n = sizeof("r") - 1; char *tb_c728r = tb_mem(tb_c728_0n + tb_c728_1n + tb_c728_2n + 1), *tb_c728rw = tb_c728r; memcpy(tb_c728rw, tb_c728_0, tb_c728_0n); tb_c728rw += tb_c728_0n; memcpy(tb_c728rw, tb_c728_1, tb_c728_1n); tb_c728rw += tb_c728_1n; memcpy(tb_c728rw, tb_c728_2, tb_c728_2n); tb_c728rw += tb_c728_2n; *tb_c728rw = 0; (const char *)tb_c728r; });
    f_emit__put(v_c, ({ const char *tb_c729_0 = "char *"; long long tb_c729_0n = sizeof("char *") - 1; const char *tb_c729_1 = v_r; long long tb_c729_1n = (long long)strlen(tb_c729_1); const char *tb_c729_2 = " = tb_mem("; long long tb_c729_2n = sizeof(" = tb_mem(") - 1; const char *tb_c729_3 = v_total; long long tb_c729_3n = (long long)strlen(tb_c729_3); const char *tb_c729_4 = " + 1), *"; long long tb_c729_4n = sizeof(" + 1), *") - 1; const char *tb_c729_5 = v_r; long long tb_c729_5n = (long long)strlen(tb_c729_5); const char *tb_c729_6 = "w = "; long long tb_c729_6n = sizeof("w = ") - 1; const char *tb_c729_7 = v_r; long long tb_c729_7n = (long long)strlen(tb_c729_7); const char *tb_c729_8 = "; "; long long tb_c729_8n = sizeof("; ") - 1; char *tb_c729r = tb_mem(tb_c729_0n + tb_c729_1n + tb_c729_2n + tb_c729_3n + tb_c729_4n + tb_c729_5n + tb_c729_6n + tb_c729_7n + tb_c729_8n + 1), *tb_c729rw = tb_c729r; memcpy(tb_c729rw, tb_c729_0, tb_c729_0n); tb_c729rw += tb_c729_0n; memcpy(tb_c729rw, tb_c729_1, tb_c729_1n); tb_c729rw += tb_c729_1n; memcpy(tb_c729rw, tb_c729_2, tb_c729_2n); tb_c729rw += tb_c729_2n; memcpy(tb_c729rw, tb_c729_3, tb_c729_3n); tb_c729rw += tb_c729_3n; memcpy(tb_c729rw, tb_c729_4, tb_c729_4n); tb_c729rw += tb_c729_4n; memcpy(tb_c729rw, tb_c729_5, tb_c729_5n); tb_c729rw += tb_c729_5n; memcpy(tb_c729rw, tb_c729_6, tb_c729_6n); tb_c729rw += tb_c729_6n; memcpy(tb_c729rw, tb_c729_7, tb_c729_7n); tb_c729rw += tb_c729_7n; memcpy(tb_c729rw, tb_c729_8, tb_c729_8n); tb_c729rw += tb_c729_8n; *tb_c729rw = 0; (const char *)tb_c729r; }));
    {
        long long tb_end730 = ((v_parts)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end730; v_i++) {
            const char * v_v = ({ const char *tb_c731_0 = "tb_c"; long long tb_c731_0n = sizeof("tb_c") - 1; const char *tb_c731_1 = v_id; long long tb_c731_1n = (long long)strlen(tb_c731_1); const char *tb_c731_2 = "_"; long long tb_c731_2n = sizeof("_") - 1; char tb_c731_3d[24]; long long tb_c731_3n; const char *tb_c731_3 = tb_digits(v_i, tb_c731_3d, &tb_c731_3n); char *tb_c731r = tb_mem(tb_c731_0n + tb_c731_1n + tb_c731_2n + tb_c731_3n + 1), *tb_c731rw = tb_c731r; memcpy(tb_c731rw, tb_c731_0, tb_c731_0n); tb_c731rw += tb_c731_0n; memcpy(tb_c731rw, tb_c731_1, tb_c731_1n); tb_c731rw += tb_c731_1n; memcpy(tb_c731rw, tb_c731_2, tb_c731_2n); tb_c731rw += tb_c731_2n; memcpy(tb_c731rw, tb_c731_3, tb_c731_3n); tb_c731rw += tb_c731_3n; *tb_c731rw = 0; (const char *)tb_c731r; });
            f_emit__put(v_c, ({ const char *tb_c732_0 = "memcpy("; long long tb_c732_0n = sizeof("memcpy(") - 1; const char *tb_c732_1 = v_r; long long tb_c732_1n = (long long)strlen(tb_c732_1); const char *tb_c732_2 = "w, "; long long tb_c732_2n = sizeof("w, ") - 1; const char *tb_c732_3 = v_v; long long tb_c732_3n = (long long)strlen(tb_c732_3); const char *tb_c732_4 = ", "; long long tb_c732_4n = sizeof(", ") - 1; const char *tb_c732_5 = v_v; long long tb_c732_5n = (long long)strlen(tb_c732_5); const char *tb_c732_6 = "n); "; long long tb_c732_6n = sizeof("n); ") - 1; const char *tb_c732_7 = v_r; long long tb_c732_7n = (long long)strlen(tb_c732_7); const char *tb_c732_8 = "w += "; long long tb_c732_8n = sizeof("w += ") - 1; const char *tb_c732_9 = v_v; long long tb_c732_9n = (long long)strlen(tb_c732_9); const char *tb_c732_10 = "n; "; long long tb_c732_10n = sizeof("n; ") - 1; char *tb_c732r = tb_mem(tb_c732_0n + tb_c732_1n + tb_c732_2n + tb_c732_3n + tb_c732_4n + tb_c732_5n + tb_c732_6n + tb_c732_7n + tb_c732_8n + tb_c732_9n + tb_c732_10n + 1), *tb_c732rw = tb_c732r; memcpy(tb_c732rw, tb_c732_0, tb_c732_0n); tb_c732rw += tb_c732_0n; memcpy(tb_c732rw, tb_c732_1, tb_c732_1n); tb_c732rw += tb_c732_1n; memcpy(tb_c732rw, tb_c732_2, tb_c732_2n); tb_c732rw += tb_c732_2n; memcpy(tb_c732rw, tb_c732_3, tb_c732_3n); tb_c732rw += tb_c732_3n; memcpy(tb_c732rw, tb_c732_4, tb_c732_4n); tb_c732rw += tb_c732_4n; memcpy(tb_c732rw, tb_c732_5, tb_c732_5n); tb_c732rw += tb_c732_5n; memcpy(tb_c732rw, tb_c732_6, tb_c732_6n); tb_c732rw += tb_c732_6n; memcpy(tb_c732rw, tb_c732_7, tb_c732_7n); tb_c732rw += tb_c732_7n; memcpy(tb_c732rw, tb_c732_8, tb_c732_8n); tb_c732rw += tb_c732_8n; memcpy(tb_c732rw, tb_c732_9, tb_c732_9n); tb_c732rw += tb_c732_9n; memcpy(tb_c732rw, tb_c732_10, tb_c732_10n); tb_c732rw += tb_c732_10n; *tb_c732rw = 0; (const char *)tb_c732r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c733_0 = "*"; long long tb_c733_0n = sizeof("*") - 1; const char *tb_c733_1 = v_r; long long tb_c733_1n = (long long)strlen(tb_c733_1); const char *tb_c733_2 = "w = 0; (const char *)"; long long tb_c733_2n = sizeof("w = 0; (const char *)") - 1; const char *tb_c733_3 = v_r; long long tb_c733_3n = (long long)strlen(tb_c733_3); const char *tb_c733_4 = "; })"; long long tb_c733_4n = sizeof("; })") - 1; char *tb_c733r = tb_mem(tb_c733_0n + tb_c733_1n + tb_c733_2n + tb_c733_3n + tb_c733_4n + 1), *tb_c733rw = tb_c733r; memcpy(tb_c733rw, tb_c733_0, tb_c733_0n); tb_c733rw += tb_c733_0n; memcpy(tb_c733rw, tb_c733_1, tb_c733_1n); tb_c733rw += tb_c733_1n; memcpy(tb_c733rw, tb_c733_2, tb_c733_2n); tb_c733rw += tb_c733_2n; memcpy(tb_c733rw, tb_c733_3, tb_c733_3n); tb_c733rw += tb_c733_3n; memcpy(tb_c733rw, tb_c733_4, tb_c733_4n); tb_c733rw += tb_c733_4n; *tb_c733rw = 0; (const char *)tb_c733r; }));
}

static void f_emit__emit_binop(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 378))->q_value;
    tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 379))->q_kids, 0LL, sizeof(tb_o_Node *), 379));
    tb_o_Node * v_b = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 380))->q_kids, 1LL, sizeof(tb_o_Node *), 380));
    if (({ const char *tb_s734 = v_op; ((unsigned char)tb_s734[0] == 111 && (unsigned char)tb_s734[1] == 114 && (unsigned char)tb_s734[2] == 101 && (unsigned char)tb_s734[3] == 108 && (unsigned char)tb_s734[4] == 115 && (unsigned char)tb_s734[5] == 101 && tb_s734[6] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c735_0 = "(tb_t"; long long tb_c735_0n = sizeof("(tb_t") - 1; char tb_c735_1d[24]; long long tb_c735_1n; const char *tb_c735_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 383))->q_temp_no, tb_c735_1d, &tb_c735_1n); const char *tb_c735_2 = " = "; long long tb_c735_2n = sizeof(" = ") - 1; char *tb_c735r = tb_mem(tb_c735_0n + tb_c735_1n + tb_c735_2n + 1), *tb_c735rw = tb_c735r; memcpy(tb_c735rw, tb_c735_0, tb_c735_0n); tb_c735rw += tb_c735_0n; memcpy(tb_c735rw, tb_c735_1, tb_c735_1n); tb_c735rw += tb_c735_1n; memcpy(tb_c735rw, tb_c735_2, tb_c735_2n); tb_c735rw += tb_c735_2n; *tb_c735rw = 0; (const char *)tb_c735r; }));
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ", tb_err ? (tb_err = 0, ");
        f_emit__emit_as(v_c, v_b, ((tb_o_Node *)tb_nn(v_n, 386))->q_ty);
        f_emit__put(v_c, ({ const char *tb_c736_0 = ") : tb_t"; long long tb_c736_0n = sizeof(") : tb_t") - 1; char tb_c736_1d[24]; long long tb_c736_1n; const char *tb_c736_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 387))->q_temp_no, tb_c736_1d, &tb_c736_1n); const char *tb_c736_2 = ")"; long long tb_c736_2n = sizeof(")") - 1; char *tb_c736r = tb_mem(tb_c736_0n + tb_c736_1n + tb_c736_2n + 1), *tb_c736rw = tb_c736r; memcpy(tb_c736rw, tb_c736_0, tb_c736_0n); tb_c736rw += tb_c736_0n; memcpy(tb_c736rw, tb_c736_1, tb_c736_1n); tb_c736rw += tb_c736_1n; memcpy(tb_c736rw, tb_c736_2, tb_c736_2n); tb_c736rw += tb_c736_2n; *tb_c736rw = 0; (const char *)tb_c736r; }));
        return;
    }
    if (({ const char *tb_s737 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 389))->q_ty); ((unsigned char)tb_s737[0] == 116 && (unsigned char)tb_s737[1] == 101 && (unsigned char)tb_s737[2] == 120 && (unsigned char)tb_s737[3] == 116 && tb_s737[4] == 0); })) {
        f_emit__emit_concat(v_c, v_n);
        return;
    }
    if (((({ const char *tb_s738 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 392))->q_ty); ((unsigned char)tb_s738[0] == 116 && (unsigned char)tb_s738[1] == 101 && (unsigned char)tb_s738[2] == 120 && (unsigned char)tb_s738[3] == 116 && tb_s738[4] == 0); }) && (({ const char *tb_s739 = v_op; ((unsigned char)tb_s739[0] == 61 && (unsigned char)tb_s739[1] == 61 && tb_s739[2] == 0); }) || ({ const char *tb_s740 = v_op; ((unsigned char)tb_s740[0] == 33 && (unsigned char)tb_s740[1] == 61 && tb_s740[2] == 0); }))) && (f_emit__short_literal(v_b) || f_emit__short_literal(v_a)))) {
        tb_o_Node * v_lit = v_b;
        tb_o_Node * v_other = v_a;
        if ((!f_emit__short_literal(v_b))) {
            v_lit = v_a;
            v_other = v_b;
        }
        const char * v_body = tb_cut(((tb_o_Node *)tb_nn(v_lit, 399))->q_value, 1LL, (tb_tlen(((tb_o_Node *)tb_nn(v_lit, 399))->q_value) - 2LL), 399);
        { long long tb_v741 = 1LL; long long *tb_p741 = &(((tb_o_Comp *)tb_nn(v_c, 400))->q_label); *tb_p741 += tb_v741; }
        const char * v_sv = ({ const char *tb_c742_0 = "tb_s"; long long tb_c742_0n = sizeof("tb_s") - 1; char tb_c742_1d[24]; long long tb_c742_1n; const char *tb_c742_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 401))->q_label, tb_c742_1d, &tb_c742_1n); char *tb_c742r = tb_mem(tb_c742_0n + tb_c742_1n + 1), *tb_c742rw = tb_c742r; memcpy(tb_c742rw, tb_c742_0, tb_c742_0n); tb_c742rw += tb_c742_0n; memcpy(tb_c742rw, tb_c742_1, tb_c742_1n); tb_c742rw += tb_c742_1n; *tb_c742rw = 0; (const char *)tb_c742r; });
        f_emit__put(v_c, ({ const char *tb_c743_0 = "({ const char *"; long long tb_c743_0n = sizeof("({ const char *") - 1; const char *tb_c743_1 = v_sv; long long tb_c743_1n = (long long)strlen(tb_c743_1); const char *tb_c743_2 = " = "; long long tb_c743_2n = sizeof(" = ") - 1; char *tb_c743r = tb_mem(tb_c743_0n + tb_c743_1n + tb_c743_2n + 1), *tb_c743rw = tb_c743r; memcpy(tb_c743rw, tb_c743_0, tb_c743_0n); tb_c743rw += tb_c743_0n; memcpy(tb_c743rw, tb_c743_1, tb_c743_1n); tb_c743rw += tb_c743_1n; memcpy(tb_c743rw, tb_c743_2, tb_c743_2n); tb_c743rw += tb_c743_2n; *tb_c743rw = 0; (const char *)tb_c743r; }));
        f_emit__emit_q(v_c, v_other);
        f_emit__put(v_c, "; ");
        if (({ const char *tb_s744 = v_op; ((unsigned char)tb_s744[0] == 33 && (unsigned char)tb_s744[1] == 61 && tb_s744[2] == 0); })) {
            f_emit__put(v_c, "!");
        }
        f_emit__put(v_c, "(");
        long long v_i = 0LL;
        {
            const char * tb_l745 = v_body;
            for (long long tb_i745 = 0; tb_l745[tb_i745] != 0; tb_i745++) {
                const char * const v_ch = tb_chr((unsigned char)tb_l745[tb_i745]);
                f_emit__put(v_c, ({ const char *tb_c746_0 = "(unsigned char)"; long long tb_c746_0n = sizeof("(unsigned char)") - 1; const char *tb_c746_1 = v_sv; long long tb_c746_1n = (long long)strlen(tb_c746_1); const char *tb_c746_2 = "["; long long tb_c746_2n = sizeof("[") - 1; char tb_c746_3d[24]; long long tb_c746_3n; const char *tb_c746_3 = tb_digits(v_i, tb_c746_3d, &tb_c746_3n); const char *tb_c746_4 = "] == "; long long tb_c746_4n = sizeof("] == ") - 1; char tb_c746_5d[24]; long long tb_c746_5n; const char *tb_c746_5 = tb_digits(tb_code(v_ch, 410), tb_c746_5d, &tb_c746_5n); const char *tb_c746_6 = " && "; long long tb_c746_6n = sizeof(" && ") - 1; char *tb_c746r = tb_mem(tb_c746_0n + tb_c746_1n + tb_c746_2n + tb_c746_3n + tb_c746_4n + tb_c746_5n + tb_c746_6n + 1), *tb_c746rw = tb_c746r; memcpy(tb_c746rw, tb_c746_0, tb_c746_0n); tb_c746rw += tb_c746_0n; memcpy(tb_c746rw, tb_c746_1, tb_c746_1n); tb_c746rw += tb_c746_1n; memcpy(tb_c746rw, tb_c746_2, tb_c746_2n); tb_c746rw += tb_c746_2n; memcpy(tb_c746rw, tb_c746_3, tb_c746_3n); tb_c746rw += tb_c746_3n; memcpy(tb_c746rw, tb_c746_4, tb_c746_4n); tb_c746rw += tb_c746_4n; memcpy(tb_c746rw, tb_c746_5, tb_c746_5n); tb_c746rw += tb_c746_5n; memcpy(tb_c746rw, tb_c746_6, tb_c746_6n); tb_c746rw += tb_c746_6n; *tb_c746rw = 0; (const char *)tb_c746r; }));
                { long long tb_v747 = 1LL; long long *tb_p747 = &(v_i); *tb_p747 += tb_v747; }
            }
        }
        f_emit__put(v_c, ({ const char *tb_c748_0 = v_sv; long long tb_c748_0n = (long long)strlen(tb_c748_0); const char *tb_c748_1 = "["; long long tb_c748_1n = sizeof("[") - 1; char tb_c748_2d[24]; long long tb_c748_2n; const char *tb_c748_2 = tb_digits(v_i, tb_c748_2d, &tb_c748_2n); const char *tb_c748_3 = "] == 0); })"; long long tb_c748_3n = sizeof("] == 0); })") - 1; char *tb_c748r = tb_mem(tb_c748_0n + tb_c748_1n + tb_c748_2n + tb_c748_3n + 1), *tb_c748rw = tb_c748r; memcpy(tb_c748rw, tb_c748_0, tb_c748_0n); tb_c748rw += tb_c748_0n; memcpy(tb_c748rw, tb_c748_1, tb_c748_1n); tb_c748rw += tb_c748_1n; memcpy(tb_c748rw, tb_c748_2, tb_c748_2n); tb_c748rw += tb_c748_2n; memcpy(tb_c748rw, tb_c748_3, tb_c748_3n); tb_c748rw += tb_c748_3n; *tb_c748rw = 0; (const char *)tb_c748r; }));
        return;
    }
    if (({ const char *tb_s749 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 414))->q_ty); ((unsigned char)tb_s749[0] == 116 && (unsigned char)tb_s749[1] == 101 && (unsigned char)tb_s749[2] == 120 && (unsigned char)tb_s749[3] == 116 && tb_s749[4] == 0); })) {
        f_emit__put(v_c, "(strcmp(");
        f_emit__emit_q(v_c, v_a);
        f_emit__put(v_c, ", ");
        f_emit__emit_q(v_c, v_b);
        f_emit__put(v_c, ({ const char *tb_c750_0 = ") "; long long tb_c750_0n = sizeof(") ") - 1; const char *tb_c750_1 = v_op; long long tb_c750_1n = (long long)strlen(tb_c750_1); const char *tb_c750_2 = " 0)"; long long tb_c750_2n = sizeof(" 0)") - 1; char *tb_c750r = tb_mem(tb_c750_0n + tb_c750_1n + tb_c750_2n + 1), *tb_c750rw = tb_c750r; memcpy(tb_c750rw, tb_c750_0, tb_c750_0n); tb_c750rw += tb_c750_0n; memcpy(tb_c750rw, tb_c750_1, tb_c750_1n); tb_c750rw += tb_c750_1n; memcpy(tb_c750rw, tb_c750_2, tb_c750_2n); tb_c750rw += tb_c750_2n; *tb_c750rw = 0; (const char *)tb_c750r; }));
        return;
    }
    if ((({ const char *tb_s751 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 421))->q_ty); ((unsigned char)tb_s751[0] == 105 && (unsigned char)tb_s751[1] == 110 && (unsigned char)tb_s751[2] == 116 && tb_s751[3] == 0); }) && (({ const char *tb_s752 = v_op; ((unsigned char)tb_s752[0] == 47 && tb_s752[1] == 0); }) || ({ const char *tb_s753 = v_op; ((unsigned char)tb_s753[0] == 37 && tb_s753[1] == 0); })))) {
        if (({ const char *tb_s754 = v_op; ((unsigned char)tb_s754[0] == 47 && tb_s754[1] == 0); })) {
            f_emit__put(v_c, "tb_div(");
        } else {
            f_emit__put(v_c, "tb_mod(");
        }
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, v_b);
        f_emit__put(v_c, ({ const char *tb_c755_0 = ", "; long long tb_c755_0n = sizeof(", ") - 1; char tb_c755_1d[24]; long long tb_c755_1n; const char *tb_c755_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 429))->q_line, tb_c755_1d, &tb_c755_1n); const char *tb_c755_2 = ")"; long long tb_c755_2n = sizeof(")") - 1; char *tb_c755r = tb_mem(tb_c755_0n + tb_c755_1n + tb_c755_2n + 1), *tb_c755rw = tb_c755r; memcpy(tb_c755rw, tb_c755_0, tb_c755_0n); tb_c755rw += tb_c755_0n; memcpy(tb_c755rw, tb_c755_1, tb_c755_1n); tb_c755rw += tb_c755_1n; memcpy(tb_c755rw, tb_c755_2, tb_c755_2n); tb_c755rw += tb_c755_2n; *tb_c755rw = 0; (const char *)tb_c755r; }));
        return;
    }
    const char * v_c_op = v_op;
    if (({ const char *tb_s756 = v_op; ((unsigned char)tb_s756[0] == 97 && (unsigned char)tb_s756[1] == 110 && (unsigned char)tb_s756[2] == 100 && tb_s756[3] == 0); })) {
        v_c_op = "&&";
    } else if (({ const char *tb_s757 = v_op; ((unsigned char)tb_s757[0] == 111 && (unsigned char)tb_s757[1] == 114 && tb_s757[2] == 0); })) {
        v_c_op = "||";
    }
    f_emit__put(v_c, "(");
    if ((({ const char *tb_s758 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 437))->q_ty); ((unsigned char)tb_s758[0] == 100 && (unsigned char)tb_s758[1] == 101 && (unsigned char)tb_s758[2] == 99 && tb_s758[3] == 0); }) && ({ const char *tb_s759 = v_op; ((unsigned char)tb_s759[0] == 47 && tb_s759[1] == 0); }))) {
        f_emit__emit_as(v_c, v_a, ((tb_o_Node *)tb_nn(v_n, 438))->q_ty);
    } else {
        f_emit__emit_expr(v_c, v_a);
    }
    f_emit__put(v_c, ({ const char *tb_c760_0 = " "; long long tb_c760_0n = sizeof(" ") - 1; const char *tb_c760_1 = v_c_op; long long tb_c760_1n = (long long)strlen(tb_c760_1); const char *tb_c760_2 = " "; long long tb_c760_2n = sizeof(" ") - 1; char *tb_c760r = tb_mem(tb_c760_0n + tb_c760_1n + tb_c760_2n + 1), *tb_c760rw = tb_c760r; memcpy(tb_c760rw, tb_c760_0, tb_c760_0n); tb_c760rw += tb_c760_0n; memcpy(tb_c760rw, tb_c760_1, tb_c760_1n); tb_c760rw += tb_c760_1n; memcpy(tb_c760rw, tb_c760_2, tb_c760_2n); tb_c760rw += tb_c760_2n; *tb_c760rw = 0; (const char *)tb_c760r; }));
    f_emit__emit_expr(v_c, v_b);
    f_emit__put(v_c, ")");
}

static void f_emit__emit_expr(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 446))->q_kind;
    if (({ const char *tb_s761 = v_k; ((unsigned char)tb_s761[0] == 110 && (unsigned char)tb_s761[1] == 117 && (unsigned char)tb_s761[2] == 109 && (unsigned char)tb_s761[3] == 98 && (unsigned char)tb_s761[4] == 101 && (unsigned char)tb_s761[5] == 114 && tb_s761[6] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c762_0 = ((tb_o_Node *)tb_nn(v_n, 448))->q_value; long long tb_c762_0n = (long long)strlen(tb_c762_0); const char *tb_c762_1 = "LL"; long long tb_c762_1n = sizeof("LL") - 1; char *tb_c762r = tb_mem(tb_c762_0n + tb_c762_1n + 1), *tb_c762rw = tb_c762r; memcpy(tb_c762rw, tb_c762_0, tb_c762_0n); tb_c762rw += tb_c762_0n; memcpy(tb_c762rw, tb_c762_1, tb_c762_1n); tb_c762rw += tb_c762_1n; *tb_c762rw = 0; (const char *)tb_c762r; }));
    } else if ((({ const char *tb_s763 = v_k; ((unsigned char)tb_s763[0] == 100 && (unsigned char)tb_s763[1] == 101 && (unsigned char)tb_s763[2] == 99 && (unsigned char)tb_s763[3] == 105 && (unsigned char)tb_s763[4] == 109 && (unsigned char)tb_s763[5] == 97 && (unsigned char)tb_s763[6] == 108 && tb_s763[7] == 0); }) || ({ const char *tb_s764 = v_k; ((unsigned char)tb_s764[0] == 115 && (unsigned char)tb_s764[1] == 116 && (unsigned char)tb_s764[2] == 114 && (unsigned char)tb_s764[3] == 105 && (unsigned char)tb_s764[4] == 110 && (unsigned char)tb_s764[5] == 103 && tb_s764[6] == 0); }))) {
        f_emit__put(v_c, ((tb_o_Node *)tb_nn(v_n, 450))->q_value);
    } else if (({ const char *tb_s765 = v_k; ((unsigned char)tb_s765[0] == 98 && (unsigned char)tb_s765[1] == 111 && (unsigned char)tb_s765[2] == 111 && (unsigned char)tb_s765[3] == 108 && tb_s765[4] == 0); })) {
        if (({ const char *tb_s766 = ((tb_o_Node *)tb_nn(v_n, 452))->q_value; ((unsigned char)tb_s766[0] == 116 && (unsigned char)tb_s766[1] == 114 && (unsigned char)tb_s766[2] == 117 && (unsigned char)tb_s766[3] == 101 && tb_s766[4] == 0); })) {
            f_emit__put(v_c, "1");
        } else {
            f_emit__put(v_c, "0");
        }
    } else if (((({ const char *tb_s767 = v_k; ((unsigned char)tb_s767[0] == 110 && (unsigned char)tb_s767[1] == 97 && (unsigned char)tb_s767[2] == 109 && (unsigned char)tb_s767[3] == 101 && tb_s767[4] == 0); }) && ((((tb_o_Comp *)tb_nn(v_c, 456))->q_vec_names)->len > 0LL)) && (f_emit__vec_index(v_c, ((tb_o_Node *)tb_nn(v_n, 456))->q_name) >= 0LL))) {
        f_emit__put(v_c, (*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 457))->q_vec_repl, f_emit__vec_index(v_c, ((tb_o_Node *)tb_nn(v_n, 457))->q_name), sizeof(const char *), 457)));
    } else if (({ const char *tb_s768 = v_k; ((unsigned char)tb_s768[0] == 110 && (unsigned char)tb_s768[1] == 97 && (unsigned char)tb_s768[2] == 109 && (unsigned char)tb_s768[3] == 101 && tb_s768[4] == 0); })) {
        if (((tb_o_Node *)tb_nn(v_n, 459))->q_is_link) {
            f_emit__put(v_c, ({ const char *tb_c769_0 = "(*v_"; long long tb_c769_0n = sizeof("(*v_") - 1; const char *tb_c769_1 = ((tb_o_Node *)tb_nn(v_n, 460))->q_name; long long tb_c769_1n = (long long)strlen(tb_c769_1); const char *tb_c769_2 = ")"; long long tb_c769_2n = sizeof(")") - 1; char *tb_c769r = tb_mem(tb_c769_0n + tb_c769_1n + tb_c769_2n + 1), *tb_c769rw = tb_c769r; memcpy(tb_c769rw, tb_c769_0, tb_c769_0n); tb_c769rw += tb_c769_0n; memcpy(tb_c769rw, tb_c769_1, tb_c769_1n); tb_c769rw += tb_c769_1n; memcpy(tb_c769rw, tb_c769_2, tb_c769_2n); tb_c769rw += tb_c769_2n; *tb_c769rw = 0; (const char *)tb_c769r; }));
        } else if (f_emit__is_bld(v_c, v_n)) {
            f_emit__put(v_c, ({ const char *tb_c770_0 = "(tb_rl_"; long long tb_c770_0n = sizeof("(tb_rl_") - 1; const char *tb_c770_1 = ((tb_o_Node *)tb_nn(v_n, 462))->q_name; long long tb_c770_1n = (long long)strlen(tb_c770_1); const char *tb_c770_2 = " = -1, v_"; long long tb_c770_2n = sizeof(" = -1, v_") - 1; const char *tb_c770_3 = ((tb_o_Node *)tb_nn(v_n, 462))->q_name; long long tb_c770_3n = (long long)strlen(tb_c770_3); const char *tb_c770_4 = ")"; long long tb_c770_4n = sizeof(")") - 1; char *tb_c770r = tb_mem(tb_c770_0n + tb_c770_1n + tb_c770_2n + tb_c770_3n + tb_c770_4n + 1), *tb_c770rw = tb_c770r; memcpy(tb_c770rw, tb_c770_0, tb_c770_0n); tb_c770rw += tb_c770_0n; memcpy(tb_c770rw, tb_c770_1, tb_c770_1n); tb_c770rw += tb_c770_1n; memcpy(tb_c770rw, tb_c770_2, tb_c770_2n); tb_c770rw += tb_c770_2n; memcpy(tb_c770rw, tb_c770_3, tb_c770_3n); tb_c770rw += tb_c770_3n; memcpy(tb_c770rw, tb_c770_4, tb_c770_4n); tb_c770rw += tb_c770_4n; *tb_c770rw = 0; (const char *)tb_c770r; }));
        } else {
            f_emit__put(v_c, ({ const char *tb_c771_0 = "v_"; long long tb_c771_0n = sizeof("v_") - 1; const char *tb_c771_1 = ((tb_o_Node *)tb_nn(v_n, 464))->q_name; long long tb_c771_1n = (long long)strlen(tb_c771_1); char *tb_c771r = tb_mem(tb_c771_0n + tb_c771_1n + 1), *tb_c771rw = tb_c771r; memcpy(tb_c771rw, tb_c771_0, tb_c771_0n); tb_c771rw += tb_c771_0n; memcpy(tb_c771rw, tb_c771_1, tb_c771_1n); tb_c771rw += tb_c771_1n; *tb_c771rw = 0; (const char *)tb_c771r; }));
        }
    } else if (({ const char *tb_s772 = v_k; ((unsigned char)tb_s772[0] == 108 && (unsigned char)tb_s772[1] == 105 && (unsigned char)tb_s772[2] == 115 && (unsigned char)tb_s772[3] == 116 && tb_s772[4] == 0); })) {
        f_emit__emit_list(v_c, v_n);
    } else if (({ const char *tb_s773 = v_k; ((unsigned char)tb_s773[0] == 100 && (unsigned char)tb_s773[1] == 105 && (unsigned char)tb_s773[2] == 99 && (unsigned char)tb_s773[3] == 116 && tb_s773[4] == 0); })) {
        f_emit__emit_dict(v_c, v_n);
    } else if ((({ const char *tb_s774 = v_k; ((unsigned char)tb_s774[0] == 116 && (unsigned char)tb_s774[1] == 114 && (unsigned char)tb_s774[2] == 121 && tb_s774[3] == 0); }) || ({ const char *tb_s775 = v_k; ((unsigned char)tb_s775[0] == 101 && (unsigned char)tb_s775[1] == 108 && (unsigned char)tb_s775[2] == 115 && (unsigned char)tb_s775[3] == 101 && (unsigned char)tb_s775[4] == 95 && (unsigned char)tb_s775[5] == 101 && (unsigned char)tb_s775[6] == 114 && (unsigned char)tb_s775[7] == 114 && tb_s775[8] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c776_0 = "tb_v"; long long tb_c776_0n = sizeof("tb_v") - 1; char tb_c776_1d[24]; long long tb_c776_1n; const char *tb_c776_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 471))->q_temp_no, tb_c776_1d, &tb_c776_1n); char *tb_c776r = tb_mem(tb_c776_0n + tb_c776_1n + 1), *tb_c776rw = tb_c776r; memcpy(tb_c776rw, tb_c776_0, tb_c776_0n); tb_c776rw += tb_c776_0n; memcpy(tb_c776rw, tb_c776_1, tb_c776_1n); tb_c776rw += tb_c776_1n; *tb_c776rw = 0; (const char *)tb_c776r; }));
    } else if (({ const char *tb_s777 = v_k; ((unsigned char)tb_s777[0] == 99 && (unsigned char)tb_s777[1] == 97 && (unsigned char)tb_s777[2] == 108 && (unsigned char)tb_s777[3] == 108 && tb_s777[4] == 0); })) {
        f_emit__emit_call(v_c, v_n);
    } else if (({ const char *tb_s778 = v_k; ((unsigned char)tb_s778[0] == 109 && (unsigned char)tb_s778[1] == 101 && (unsigned char)tb_s778[2] == 116 && (unsigned char)tb_s778[3] == 104 && (unsigned char)tb_s778[4] == 111 && (unsigned char)tb_s778[5] == 100 && tb_s778[6] == 0); })) {
        f_emit__emit_method(v_c, v_n);
    } else if (({ const char *tb_s779 = v_k; ((unsigned char)tb_s779[0] == 98 && (unsigned char)tb_s779[1] == 105 && (unsigned char)tb_s779[2] == 110 && (unsigned char)tb_s779[3] == 111 && (unsigned char)tb_s779[4] == 112 && tb_s779[5] == 0); })) {
        f_emit__emit_binop(v_c, v_n);
    } else if (({ const char *tb_s780 = v_k; ((unsigned char)tb_s780[0] == 117 && (unsigned char)tb_s780[1] == 110 && (unsigned char)tb_s780[2] == 97 && (unsigned char)tb_s780[3] == 114 && (unsigned char)tb_s780[4] == 121 && tb_s780[5] == 0); })) {
        if (({ const char *tb_s781 = ((tb_o_Node *)tb_nn(v_n, 479))->q_value; ((unsigned char)tb_s781[0] == 110 && (unsigned char)tb_s781[1] == 111 && (unsigned char)tb_s781[2] == 116 && tb_s781[3] == 0); })) {
            f_emit__put(v_c, "(!");
        } else {
            f_emit__put(v_c, "(-");
        }
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 483))->q_kids, 0LL, sizeof(tb_o_Node *), 483)));
        f_emit__put(v_c, ")");
    } else if ((({ const char *tb_s782 = v_k; ((unsigned char)tb_s782[0] == 105 && (unsigned char)tb_s782[1] == 110 && (unsigned char)tb_s782[2] == 100 && (unsigned char)tb_s782[3] == 101 && (unsigned char)tb_s782[4] == 120 && tb_s782[5] == 0); }) && ({ const char *tb_s783 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 485))->q_kids, 0LL, sizeof(tb_o_Node *), 485)), 485))->q_ty); ((unsigned char)tb_s783[0] == 100 && (unsigned char)tb_s783[1] == 105 && (unsigned char)tb_s783[2] == 99 && (unsigned char)tb_s783[3] == 116 && tb_s783[4] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c784_0 = "(*("; long long tb_c784_0n = sizeof("(*(") - 1; const char *tb_c784_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 486))->q_ty); long long tb_c784_1n = (long long)strlen(tb_c784_1); const char *tb_c784_2 = " *)tb_dget("; long long tb_c784_2n = sizeof(" *)tb_dget(") - 1; char *tb_c784r = tb_mem(tb_c784_0n + tb_c784_1n + tb_c784_2n + 1), *tb_c784rw = tb_c784r; memcpy(tb_c784rw, tb_c784_0, tb_c784_0n); tb_c784rw += tb_c784_0n; memcpy(tb_c784rw, tb_c784_1, tb_c784_1n); tb_c784rw += tb_c784_1n; memcpy(tb_c784rw, tb_c784_2, tb_c784_2n); tb_c784rw += tb_c784_2n; *tb_c784rw = 0; (const char *)tb_c784r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 487))->q_kids, 0LL, sizeof(tb_o_Node *), 487)));
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 489))->q_kids, 0LL, sizeof(tb_o_Node *), 489)), 489))->q_ty, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 489))->q_kids, 1LL, sizeof(tb_o_Node *), 489)));
        f_emit__put(v_c, ({ const char *tb_c785_0 = ", "; long long tb_c785_0n = sizeof(", ") - 1; char tb_c785_1d[24]; long long tb_c785_1n; const char *tb_c785_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 490))->q_line, tb_c785_1d, &tb_c785_1n); const char *tb_c785_2 = "))"; long long tb_c785_2n = sizeof("))") - 1; char *tb_c785r = tb_mem(tb_c785_0n + tb_c785_1n + tb_c785_2n + 1), *tb_c785rw = tb_c785r; memcpy(tb_c785rw, tb_c785_0, tb_c785_0n); tb_c785rw += tb_c785_0n; memcpy(tb_c785rw, tb_c785_1, tb_c785_1n); tb_c785rw += tb_c785_1n; memcpy(tb_c785rw, tb_c785_2, tb_c785_2n); tb_c785rw += tb_c785_2n; *tb_c785rw = 0; (const char *)tb_c785r; }));
    } else if ((({ const char *tb_s786 = v_k; ((unsigned char)tb_s786[0] == 105 && (unsigned char)tb_s786[1] == 110 && (unsigned char)tb_s786[2] == 100 && (unsigned char)tb_s786[3] == 101 && (unsigned char)tb_s786[4] == 120 && tb_s786[5] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 491))->q_kids, 0LL, sizeof(tb_o_Node *), 491)), 491))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c787_0 = "tb_colat_"; long long tb_c787_0n = sizeof("tb_colat_") - 1; const char *tb_c787_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 492))->q_kids, 0LL, sizeof(tb_o_Node *), 492)), 492))->q_ty); long long tb_c787_1n = (long long)strlen(tb_c787_1); const char *tb_c787_2 = "("; long long tb_c787_2n = sizeof("(") - 1; char *tb_c787r = tb_mem(tb_c787_0n + tb_c787_1n + tb_c787_2n + 1), *tb_c787rw = tb_c787r; memcpy(tb_c787rw, tb_c787_0, tb_c787_0n); tb_c787rw += tb_c787_0n; memcpy(tb_c787rw, tb_c787_1, tb_c787_1n); tb_c787rw += tb_c787_1n; memcpy(tb_c787rw, tb_c787_2, tb_c787_2n); tb_c787rw += tb_c787_2n; *tb_c787rw = 0; (const char *)tb_c787r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 493))->q_kids, 0LL, sizeof(tb_o_Node *), 493)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 495))->q_kids, 1LL, sizeof(tb_o_Node *), 495)));
        f_emit__put(v_c, ({ const char *tb_c788_0 = ", "; long long tb_c788_0n = sizeof(", ") - 1; char tb_c788_1d[24]; long long tb_c788_1n; const char *tb_c788_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 496))->q_line, tb_c788_1d, &tb_c788_1n); const char *tb_c788_2 = ")"; long long tb_c788_2n = sizeof(")") - 1; char *tb_c788r = tb_mem(tb_c788_0n + tb_c788_1n + tb_c788_2n + 1), *tb_c788rw = tb_c788r; memcpy(tb_c788rw, tb_c788_0, tb_c788_0n); tb_c788rw += tb_c788_0n; memcpy(tb_c788rw, tb_c788_1, tb_c788_1n); tb_c788rw += tb_c788_1n; memcpy(tb_c788rw, tb_c788_2, tb_c788_2n); tb_c788rw += tb_c788_2n; *tb_c788rw = 0; (const char *)tb_c788r; }));
    } else if (({ const char *tb_s789 = v_k; ((unsigned char)tb_s789[0] == 105 && (unsigned char)tb_s789[1] == 110 && (unsigned char)tb_s789[2] == 100 && (unsigned char)tb_s789[3] == 101 && (unsigned char)tb_s789[4] == 120 && tb_s789[5] == 0); })) {
        if (({ const char *tb_s790 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 498))->q_kids, 0LL, sizeof(tb_o_Node *), 498)), 498))->q_ty); ((unsigned char)tb_s790[0] == 116 && (unsigned char)tb_s790[1] == 101 && (unsigned char)tb_s790[2] == 120 && (unsigned char)tb_s790[3] == 116 && tb_s790[4] == 0); })) {
            f_emit__put(v_c, "tb_char_at(");
            f_emit__emit_q(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 500))->q_kids, 0LL, sizeof(tb_o_Node *), 500)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 502))->q_kids, 1LL, sizeof(tb_o_Node *), 502)));
            if (f_emit__is_bld(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 503))->q_kids, 0LL, sizeof(tb_o_Node *), 503)))) {
                f_emit__put(v_c, ({ const char *tb_c791_0 = ", tb_rl_"; long long tb_c791_0n = sizeof(", tb_rl_") - 1; const char *tb_c791_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 504))->q_kids, 0LL, sizeof(tb_o_Node *), 504)), 504))->q_name; long long tb_c791_1n = (long long)strlen(tb_c791_1); char *tb_c791r = tb_mem(tb_c791_0n + tb_c791_1n + 1), *tb_c791rw = tb_c791r; memcpy(tb_c791rw, tb_c791_0, tb_c791_0n); tb_c791rw += tb_c791_0n; memcpy(tb_c791rw, tb_c791_1, tb_c791_1n); tb_c791rw += tb_c791_1n; *tb_c791rw = 0; (const char *)tb_c791r; }));
            } else {
                f_emit__put(v_c, ", -1");
            }
            f_emit__put(v_c, ({ const char *tb_c792_0 = ", "; long long tb_c792_0n = sizeof(", ") - 1; char tb_c792_1d[24]; long long tb_c792_1n; const char *tb_c792_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 507))->q_line, tb_c792_1d, &tb_c792_1n); const char *tb_c792_2 = ")"; long long tb_c792_2n = sizeof(")") - 1; char *tb_c792r = tb_mem(tb_c792_0n + tb_c792_1n + tb_c792_2n + 1), *tb_c792rw = tb_c792r; memcpy(tb_c792rw, tb_c792_0, tb_c792_0n); tb_c792rw += tb_c792_0n; memcpy(tb_c792rw, tb_c792_1, tb_c792_1n); tb_c792rw += tb_c792_1n; memcpy(tb_c792rw, tb_c792_2, tb_c792_2n); tb_c792rw += tb_c792_2n; *tb_c792rw = 0; (const char *)tb_c792r; }));
        } else if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 508))->q_kids, 0LL, sizeof(tb_o_Node *), 508)), 508))->q_ty)) {
            f_emit__put(v_c, "tb_bget(");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 510))->q_kids, 0LL, sizeof(tb_o_Node *), 510)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 512))->q_kids, 1LL, sizeof(tb_o_Node *), 512)));
            f_emit__put(v_c, ({ const char *tb_c793_0 = ", "; long long tb_c793_0n = sizeof(", ") - 1; char tb_c793_1d[24]; long long tb_c793_1n; const char *tb_c793_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 513))->q_line, tb_c793_1d, &tb_c793_1n); const char *tb_c793_2 = ")"; long long tb_c793_2n = sizeof(")") - 1; char *tb_c793r = tb_mem(tb_c793_0n + tb_c793_1n + tb_c793_2n + 1), *tb_c793rw = tb_c793r; memcpy(tb_c793rw, tb_c793_0, tb_c793_0n); tb_c793rw += tb_c793_0n; memcpy(tb_c793rw, tb_c793_1, tb_c793_1n); tb_c793rw += tb_c793_1n; memcpy(tb_c793rw, tb_c793_2, tb_c793_2n); tb_c793rw += tb_c793_2n; *tb_c793rw = 0; (const char *)tb_c793r; }));
        } else {
            const char * v_t = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 515))->q_ty);
            f_emit__put(v_c, ({ const char *tb_c794_0 = "(*("; long long tb_c794_0n = sizeof("(*(") - 1; const char *tb_c794_1 = v_t; long long tb_c794_1n = (long long)strlen(tb_c794_1); const char *tb_c794_2 = " *)tb_at("; long long tb_c794_2n = sizeof(" *)tb_at(") - 1; char *tb_c794r = tb_mem(tb_c794_0n + tb_c794_1n + tb_c794_2n + 1), *tb_c794rw = tb_c794r; memcpy(tb_c794rw, tb_c794_0, tb_c794_0n); tb_c794rw += tb_c794_0n; memcpy(tb_c794rw, tb_c794_1, tb_c794_1n); tb_c794rw += tb_c794_1n; memcpy(tb_c794rw, tb_c794_2, tb_c794_2n); tb_c794rw += tb_c794_2n; *tb_c794rw = 0; (const char *)tb_c794r; }));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 517))->q_kids, 0LL, sizeof(tb_o_Node *), 517)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 519))->q_kids, 1LL, sizeof(tb_o_Node *), 519)));
            f_emit__put(v_c, ({ const char *tb_c795_0 = ", sizeof("; long long tb_c795_0n = sizeof(", sizeof(") - 1; const char *tb_c795_1 = v_t; long long tb_c795_1n = (long long)strlen(tb_c795_1); const char *tb_c795_2 = "), "; long long tb_c795_2n = sizeof("), ") - 1; char tb_c795_3d[24]; long long tb_c795_3n; const char *tb_c795_3 = tb_digits(((tb_o_Node *)tb_nn(v_n, 520))->q_line, tb_c795_3d, &tb_c795_3n); const char *tb_c795_4 = "))"; long long tb_c795_4n = sizeof("))") - 1; char *tb_c795r = tb_mem(tb_c795_0n + tb_c795_1n + tb_c795_2n + tb_c795_3n + tb_c795_4n + 1), *tb_c795rw = tb_c795r; memcpy(tb_c795rw, tb_c795_0, tb_c795_0n); tb_c795rw += tb_c795_0n; memcpy(tb_c795rw, tb_c795_1, tb_c795_1n); tb_c795rw += tb_c795_1n; memcpy(tb_c795rw, tb_c795_2, tb_c795_2n); tb_c795rw += tb_c795_2n; memcpy(tb_c795rw, tb_c795_3, tb_c795_3n); tb_c795rw += tb_c795_3n; memcpy(tb_c795rw, tb_c795_4, tb_c795_4n); tb_c795rw += tb_c795_4n; *tb_c795rw = 0; (const char *)tb_c795r; }));
        }
    } else if ((({ const char *tb_s796 = v_k; ((unsigned char)tb_s796[0] == 102 && (unsigned char)tb_s796[1] == 105 && (unsigned char)tb_s796[2] == 101 && (unsigned char)tb_s796[3] == 108 && (unsigned char)tb_s796[4] == 100 && tb_s796[5] == 0); }) && ({ const char *tb_s797 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 521))->q_kids, 0LL, sizeof(tb_o_Node *), 521)), 521))->q_ty); ((unsigned char)tb_s797[0] == 116 && (unsigned char)tb_s797[1] == 97 && (unsigned char)tb_s797[2] == 115 && (unsigned char)tb_s797[3] == 107 && tb_s797[4] == 0); }))) {
        long long v_rt = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 522))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 522))->q_kids, 0LL, sizeof(tb_o_Node *), 522)), 522))->q_ty, sizeof(tb_o_Type *), 522)), 522))->q_elem;
        if ((v_rt == 0LL)) {
            f_emit__put(v_c, "((void)tb_wait(");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 525))->q_kids, 0LL, sizeof(tb_o_Node *), 525)));
            f_emit__put(v_c, "))");
        } else {
            f_emit__put(v_c, ({ const char *tb_c798_0 = "(*("; long long tb_c798_0n = sizeof("(*(") - 1; const char *tb_c798_1 = f_emit__c_type(v_c, v_rt); long long tb_c798_1n = (long long)strlen(tb_c798_1); const char *tb_c798_2 = " *)tb_wait("; long long tb_c798_2n = sizeof(" *)tb_wait(") - 1; char *tb_c798r = tb_mem(tb_c798_0n + tb_c798_1n + tb_c798_2n + 1), *tb_c798rw = tb_c798r; memcpy(tb_c798rw, tb_c798_0, tb_c798_0n); tb_c798rw += tb_c798_0n; memcpy(tb_c798rw, tb_c798_1, tb_c798_1n); tb_c798rw += tb_c798_1n; memcpy(tb_c798rw, tb_c798_2, tb_c798_2n); tb_c798rw += tb_c798_2n; *tb_c798rw = 0; (const char *)tb_c798r; }));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 529))->q_kids, 0LL, sizeof(tb_o_Node *), 529)));
            f_emit__put(v_c, "))");
        }
    } else if ((strcmp(v_k, "background") == 0)) {
        f_emit__emit_background(v_c, v_n);
    } else if ((({ const char *tb_s799 = v_k; ((unsigned char)tb_s799[0] == 102 && (unsigned char)tb_s799[1] == 105 && (unsigned char)tb_s799[2] == 101 && (unsigned char)tb_s799[3] == 108 && (unsigned char)tb_s799[4] == 100 && tb_s799[5] == 0); }) && f_check__is_cols_obj(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 533))->q_kids, 0LL, sizeof(tb_o_Node *), 533)), 533))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c800_0 = "(*tb_colf_"; long long tb_c800_0n = sizeof("(*tb_colf_") - 1; const char *tb_c800_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 535))->q_kids, 0LL, sizeof(tb_o_Node *), 535)), 535))->q_ty); long long tb_c800_1n = (long long)strlen(tb_c800_1); const char *tb_c800_2 = "_"; long long tb_c800_2n = sizeof("_") - 1; const char *tb_c800_3 = ((tb_o_Node *)tb_nn(v_n, 535))->q_name; long long tb_c800_3n = (long long)strlen(tb_c800_3); const char *tb_c800_4 = "("; long long tb_c800_4n = sizeof("(") - 1; char *tb_c800r = tb_mem(tb_c800_0n + tb_c800_1n + tb_c800_2n + tb_c800_3n + tb_c800_4n + 1), *tb_c800rw = tb_c800r; memcpy(tb_c800rw, tb_c800_0, tb_c800_0n); tb_c800rw += tb_c800_0n; memcpy(tb_c800rw, tb_c800_1, tb_c800_1n); tb_c800rw += tb_c800_1n; memcpy(tb_c800rw, tb_c800_2, tb_c800_2n); tb_c800rw += tb_c800_2n; memcpy(tb_c800rw, tb_c800_3, tb_c800_3n); tb_c800rw += tb_c800_3n; memcpy(tb_c800rw, tb_c800_4, tb_c800_4n); tb_c800rw += tb_c800_4n; *tb_c800rw = 0; (const char *)tb_c800r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 536))->q_kids, 0LL, sizeof(tb_o_Node *), 536)));
        f_emit__put(v_c, ({ const char *tb_c801_0 = ", "; long long tb_c801_0n = sizeof(", ") - 1; char tb_c801_1d[24]; long long tb_c801_1n; const char *tb_c801_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 537))->q_line, tb_c801_1d, &tb_c801_1n); const char *tb_c801_2 = "))"; long long tb_c801_2n = sizeof("))") - 1; char *tb_c801r = tb_mem(tb_c801_0n + tb_c801_1n + tb_c801_2n + 1), *tb_c801rw = tb_c801r; memcpy(tb_c801rw, tb_c801_0, tb_c801_0n); tb_c801rw += tb_c801_0n; memcpy(tb_c801rw, tb_c801_1, tb_c801_1n); tb_c801rw += tb_c801_1n; memcpy(tb_c801rw, tb_c801_2, tb_c801_2n); tb_c801rw += tb_c801_2n; *tb_c801rw = 0; (const char *)tb_c801r; }));
    } else if (({ const char *tb_s802 = v_k; ((unsigned char)tb_s802[0] == 102 && (unsigned char)tb_s802[1] == 105 && (unsigned char)tb_s802[2] == 101 && (unsigned char)tb_s802[3] == 108 && (unsigned char)tb_s802[4] == 100 && tb_s802[5] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c803_0 = "(("; long long tb_c803_0n = sizeof("((") - 1; const char *tb_c803_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 539))->q_kids, 0LL, sizeof(tb_o_Node *), 539)), 539))->q_ty); long long tb_c803_1n = (long long)strlen(tb_c803_1); const char *tb_c803_2 = ")tb_nn("; long long tb_c803_2n = sizeof(")tb_nn(") - 1; char *tb_c803r = tb_mem(tb_c803_0n + tb_c803_1n + tb_c803_2n + 1), *tb_c803rw = tb_c803r; memcpy(tb_c803rw, tb_c803_0, tb_c803_0n); tb_c803rw += tb_c803_0n; memcpy(tb_c803rw, tb_c803_1, tb_c803_1n); tb_c803rw += tb_c803_1n; memcpy(tb_c803rw, tb_c803_2, tb_c803_2n); tb_c803rw += tb_c803_2n; *tb_c803rw = 0; (const char *)tb_c803r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 540))->q_kids, 0LL, sizeof(tb_o_Node *), 540)));
        f_emit__put(v_c, ({ const char *tb_c804_0 = ", "; long long tb_c804_0n = sizeof(", ") - 1; char tb_c804_1d[24]; long long tb_c804_1n; const char *tb_c804_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 541))->q_line, tb_c804_1d, &tb_c804_1n); const char *tb_c804_2 = "))"; long long tb_c804_2n = sizeof("))") - 1; const char *tb_c804_3 = f_emit__field_path(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 541))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 541))->q_kids, 0LL, sizeof(tb_o_Node *), 541)), 541))->q_ty, sizeof(tb_o_Type *), 541)), 541))->q_obj_node, ((tb_o_Node *)tb_nn(v_n, 541))->q_name); long long tb_c804_3n = (long long)strlen(tb_c804_3); char *tb_c804r = tb_mem(tb_c804_0n + tb_c804_1n + tb_c804_2n + tb_c804_3n + 1), *tb_c804rw = tb_c804r; memcpy(tb_c804rw, tb_c804_0, tb_c804_0n); tb_c804rw += tb_c804_0n; memcpy(tb_c804rw, tb_c804_1, tb_c804_1n); tb_c804rw += tb_c804_1n; memcpy(tb_c804rw, tb_c804_2, tb_c804_2n); tb_c804rw += tb_c804_2n; memcpy(tb_c804rw, tb_c804_3, tb_c804_3n); tb_c804rw += tb_c804_3n; *tb_c804rw = 0; (const char *)tb_c804r; }));
    }
}

static void f_emit__emit_bail(tb_o_Comp * v_c) {
    tb_o_Node * v_f = ((tb_o_Comp *)tb_nn(v_c, 547))->q_cur_func;
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 548))->q_kids, 2LL, sizeof(tb_o_Node *), 548));
    if (({ const char *tb_s805 = ((tb_o_Node *)tb_nn(v_f, 549))->q_name; ((unsigned char)tb_s805[0] == 109 && (unsigned char)tb_s805[1] == 97 && (unsigned char)tb_s805[2] == 105 && (unsigned char)tb_s805[3] == 110 && tb_s805[4] == 0); })) {
        f_emit__put(v_c, "tb_die(tb_err);");
    } else if (((((tb_o_Node *)tb_nn(v_outputs, 551))->q_kids)->len == 1LL)) {
        f_emit__put(v_c, ({ const char *tb_c806_0 = "return v_"; long long tb_c806_0n = sizeof("return v_") - 1; const char *tb_c806_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 552))->q_kids, 0LL, sizeof(tb_o_Node *), 552)), 552))->q_name; long long tb_c806_1n = (long long)strlen(tb_c806_1); const char *tb_c806_2 = ";"; long long tb_c806_2n = sizeof(";") - 1; char *tb_c806r = tb_mem(tb_c806_0n + tb_c806_1n + tb_c806_2n + 1), *tb_c806rw = tb_c806r; memcpy(tb_c806rw, tb_c806_0, tb_c806_0n); tb_c806rw += tb_c806_0n; memcpy(tb_c806rw, tb_c806_1, tb_c806_1n); tb_c806rw += tb_c806_1n; memcpy(tb_c806rw, tb_c806_2, tb_c806_2n); tb_c806rw += tb_c806_2n; *tb_c806rw = 0; (const char *)tb_c806r; }));
    } else if (((((tb_o_Node *)tb_nn(v_outputs, 553))->q_kids)->len > 1LL)) {
        f_emit__put(v_c, ({ const char *tb_c807_0 = "return ("; long long tb_c807_0n = sizeof("return (") - 1; const char *tb_c807_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 554))->q_ty); long long tb_c807_1n = (long long)strlen(tb_c807_1); const char *tb_c807_2 = "){"; long long tb_c807_2n = sizeof("){") - 1; char *tb_c807r = tb_mem(tb_c807_0n + tb_c807_1n + tb_c807_2n + 1), *tb_c807rw = tb_c807r; memcpy(tb_c807rw, tb_c807_0, tb_c807_0n); tb_c807rw += tb_c807_0n; memcpy(tb_c807rw, tb_c807_1, tb_c807_1n); tb_c807rw += tb_c807_1n; memcpy(tb_c807rw, tb_c807_2, tb_c807_2n); tb_c807rw += tb_c807_2n; *tb_c807rw = 0; (const char *)tb_c807r; }));
        {
            long long tb_end808 = ((((tb_o_Node *)tb_nn(v_outputs, 555))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end808; v_i++) {
                if ((v_i > 0LL)) {
                    f_emit__put(v_c, ", ");
                }
                f_emit__put(v_c, ({ const char *tb_c809_0 = "v_"; long long tb_c809_0n = sizeof("v_") - 1; const char *tb_c809_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 558))->q_kids, v_i, sizeof(tb_o_Node *), 558)), 558))->q_name; long long tb_c809_1n = (long long)strlen(tb_c809_1); char *tb_c809r = tb_mem(tb_c809_0n + tb_c809_1n + 1), *tb_c809rw = tb_c809r; memcpy(tb_c809rw, tb_c809_0, tb_c809_0n); tb_c809rw += tb_c809_0n; memcpy(tb_c809rw, tb_c809_1, tb_c809_1n); tb_c809rw += tb_c809_1n; *tb_c809rw = 0; (const char *)tb_c809r; }));
            }
        }
        f_emit__put(v_c, "};");
    } else if (({ const char *tb_s810 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 560))->q_ty); ((unsigned char)tb_s810[0] == 110 && (unsigned char)tb_s810[1] == 111 && (unsigned char)tb_s810[2] == 110 && (unsigned char)tb_s810[3] == 101 && tb_s810[4] == 0); })) {
        f_emit__put(v_c, "return;");
    } else {
        f_emit__put(v_c, "return ");
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_f, 564))->q_ty);
        f_emit__put(v_c, ";");
    }
}

static void f_emit__emit_pre(tb_o_Comp * v_c, tb_o_Node * v_v, long long v_depth) {
    if ((({ const char *tb_s811 = ((tb_o_Node *)tb_nn(v_v, 569))->q_kind; !((unsigned char)tb_s811[0] == 116 && (unsigned char)tb_s811[1] == 114 && (unsigned char)tb_s811[2] == 121 && tb_s811[3] == 0); }) && ({ const char *tb_s812 = ((tb_o_Node *)tb_nn(v_v, 569))->q_kind; !((unsigned char)tb_s812[0] == 101 && (unsigned char)tb_s812[1] == 108 && (unsigned char)tb_s812[2] == 115 && (unsigned char)tb_s812[3] == 101 && (unsigned char)tb_s812[4] == 95 && (unsigned char)tb_s812[5] == 101 && (unsigned char)tb_s812[6] == 114 && (unsigned char)tb_s812[7] == 114 && tb_s812[8] == 0); }))) {
        return;
    }
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 571))->q_kids, 0LL, sizeof(tb_o_Node *), 571));
    { long long tb_v813 = 1LL; long long *tb_p813 = &(((tb_o_Comp *)tb_nn(v_c, 572))->q_label); *tb_p813 += tb_v813; }
    { long long tb_v814 = ((tb_o_Comp *)tb_nn(v_c, 573))->q_label; long long *tb_p814 = &(((tb_o_Node *)tb_nn(v_v, 573))->q_temp_no); *tb_p814 = tb_v814; }
    f_emit__indent(v_c, v_depth);
    if (({ const char *tb_s815 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_call, 575))->q_ty); !((unsigned char)tb_s815[0] == 110 && (unsigned char)tb_s815[1] == 111 && (unsigned char)tb_s815[2] == 110 && (unsigned char)tb_s815[3] == 101 && tb_s815[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c816_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_call, 576))->q_ty); long long tb_c816_0n = (long long)strlen(tb_c816_0); const char *tb_c816_1 = " tb_v"; long long tb_c816_1n = sizeof(" tb_v") - 1; char tb_c816_2d[24]; long long tb_c816_2n; const char *tb_c816_2 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 576))->q_label, tb_c816_2d, &tb_c816_2n); const char *tb_c816_3 = " = "; long long tb_c816_3n = sizeof(" = ") - 1; char *tb_c816r = tb_mem(tb_c816_0n + tb_c816_1n + tb_c816_2n + tb_c816_3n + 1), *tb_c816rw = tb_c816r; memcpy(tb_c816rw, tb_c816_0, tb_c816_0n); tb_c816rw += tb_c816_0n; memcpy(tb_c816rw, tb_c816_1, tb_c816_1n); tb_c816rw += tb_c816_1n; memcpy(tb_c816rw, tb_c816_2, tb_c816_2n); tb_c816rw += tb_c816_2n; memcpy(tb_c816rw, tb_c816_3, tb_c816_3n); tb_c816rw += tb_c816_3n; *tb_c816rw = 0; (const char *)tb_c816r; }));
    }
    f_emit__emit_expr(v_c, v_call);
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, v_depth);
    if (({ const char *tb_s817 = ((tb_o_Node *)tb_nn(v_v, 580))->q_kind; ((unsigned char)tb_s817[0] == 116 && (unsigned char)tb_s817[1] == 114 && (unsigned char)tb_s817[2] == 121 && tb_s817[3] == 0); })) {
        f_emit__put(v_c, "if (TB_RARE(tb_err != 0)) { ");
        f_emit__emit_bail(v_c);
        f_emit__put(v_c, " }\n");
        return;
    }
    f_emit__put(v_c, "if (tb_err != 0) {\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c818_0 = "const char * const v_"; long long tb_c818_0n = sizeof("const char * const v_") - 1; const char *tb_c818_1 = ((tb_o_Node *)tb_nn(v_v, 587))->q_name; long long tb_c818_1n = (long long)strlen(tb_c818_1); const char *tb_c818_2 = " = tb_err;\n"; long long tb_c818_2n = sizeof(" = tb_err;\n") - 1; char *tb_c818r = tb_mem(tb_c818_0n + tb_c818_1n + tb_c818_2n + 1), *tb_c818rw = tb_c818r; memcpy(tb_c818rw, tb_c818_0, tb_c818_0n); tb_c818rw += tb_c818_0n; memcpy(tb_c818rw, tb_c818_1, tb_c818_1n); tb_c818rw += tb_c818_1n; memcpy(tb_c818rw, tb_c818_2, tb_c818_2n); tb_c818rw += tb_c818_2n; *tb_c818rw = 0; (const char *)tb_c818r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "tb_err = 0;\n");
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 590))->q_kids, 1LL, sizeof(tb_o_Node *), 590)), (v_depth + 1LL));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_target(tb_o_Comp * v_c, tb_o_Node * v_t) {
    if ((({ const char *tb_s819 = ((tb_o_Node *)tb_nn(v_t, 596))->q_kind; ((unsigned char)tb_s819[0] == 105 && (unsigned char)tb_s819[1] == 110 && (unsigned char)tb_s819[2] == 100 && (unsigned char)tb_s819[3] == 101 && (unsigned char)tb_s819[4] == 120 && tb_s819[5] == 0); }) && ({ const char *tb_s820 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 596))->q_kids, 0LL, sizeof(tb_o_Node *), 596)), 596))->q_ty); ((unsigned char)tb_s820[0] == 100 && (unsigned char)tb_s820[1] == 105 && (unsigned char)tb_s820[2] == 99 && (unsigned char)tb_s820[3] == 116 && tb_s820[4] == 0); }))) {
        long long v_d = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 597))->q_kids, 0LL, sizeof(tb_o_Node *), 597)), 597))->q_ty;
        long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 598))->q_types, v_d, sizeof(tb_o_Type *), 598)), 598))->q_elem;
        const char * v_vt = f_emit__c_type(v_c, v_elem);
        f_emit__put(v_c, ({ const char *tb_c821_0 = "(*("; long long tb_c821_0n = sizeof("(*(") - 1; const char *tb_c821_1 = v_vt; long long tb_c821_1n = (long long)strlen(tb_c821_1); const char *tb_c821_2 = " *)tb_dput("; long long tb_c821_2n = sizeof(" *)tb_dput(") - 1; char *tb_c821r = tb_mem(tb_c821_0n + tb_c821_1n + tb_c821_2n + 1), *tb_c821rw = tb_c821r; memcpy(tb_c821rw, tb_c821_0, tb_c821_0n); tb_c821rw += tb_c821_0n; memcpy(tb_c821rw, tb_c821_1, tb_c821_1n); tb_c821rw += tb_c821_1n; memcpy(tb_c821rw, tb_c821_2, tb_c821_2n); tb_c821rw += tb_c821_2n; *tb_c821rw = 0; (const char *)tb_c821r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 601))->q_kids, 0LL, sizeof(tb_o_Node *), 601)));
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, v_d, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 603))->q_kids, 1LL, sizeof(tb_o_Node *), 603)));
        if (f_emit__is_simple(v_c, v_elem)) {
            f_emit__put(v_c, ({ const char *tb_c822_0 = ", &("; long long tb_c822_0n = sizeof(", &(") - 1; const char *tb_c822_1 = v_vt; long long tb_c822_1n = (long long)strlen(tb_c822_1); const char *tb_c822_2 = "){"; long long tb_c822_2n = sizeof("){") - 1; char *tb_c822r = tb_mem(tb_c822_0n + tb_c822_1n + tb_c822_2n + 1), *tb_c822rw = tb_c822r; memcpy(tb_c822rw, tb_c822_0, tb_c822_0n); tb_c822rw += tb_c822_0n; memcpy(tb_c822rw, tb_c822_1, tb_c822_1n); tb_c822rw += tb_c822_1n; memcpy(tb_c822rw, tb_c822_2, tb_c822_2n); tb_c822rw += tb_c822_2n; *tb_c822rw = 0; (const char *)tb_c822r; }));
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
    if (((((tb_o_Node *)tb_nn(v_n, 614))->q_kids)->len == 1LL)) {
        f_emit__emit_pre(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 615))->q_kids, 0LL, sizeof(tb_o_Node *), 615)), v_depth);
    }
    f_emit__indent(v_c, v_depth);
    _Bool v_bld = (((!((tb_o_Node *)tb_nn(v_n, 617))->q_is_link) && ({ const char *tb_s823 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 617))->q_ty); ((unsigned char)tb_s823[0] == 116 && (unsigned char)tb_s823[1] == 101 && (unsigned char)tb_s823[2] == 120 && (unsigned char)tb_s823[3] == 116 && tb_s823[4] == 0); })) && f_emit__bld_has(((tb_o_Comp *)tb_nn(v_c, 617))->q_builders, ((tb_o_Node *)tb_nn(v_n, 617))->q_name));
    if ((((v_bld && (!((tb_o_Node *)tb_nn(v_n, 618))->q_creates)) && ((((tb_o_Node *)tb_nn(v_n, 618))->q_kids)->len == 1LL)) && f_emit__app_form(v_c, ((tb_o_Node *)tb_nn(v_n, 618))->q_name, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 618))->q_kids, 0LL, sizeof(tb_o_Node *), 618))))) {
        {
            tb_list * tb_l824 = f_emit__app_parts(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 620))->q_kids, 0LL, sizeof(tb_o_Node *), 620)));
            for (long long tb_i824 = 0; tb_i824 < tb_l824->len; tb_i824++) {
                tb_o_Node * const v_part = ((tb_o_Node * *)tb_l824->data)[tb_i824];
                f_emit__emit_app(v_c, ((tb_o_Node *)tb_nn(v_n, 621))->q_name, v_part);
                f_emit__put(v_c, " ");
            }
        }
        f_emit__put(v_c, "\n");
        return;
    }
    if (((tb_o_Node *)tb_nn(v_n, 625))->q_creates) {
        f_emit__put(v_c, ({ const char *tb_c825_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 626))->q_ty); long long tb_c825_0n = (long long)strlen(tb_c825_0); const char *tb_c825_1 = " "; long long tb_c825_1n = sizeof(" ") - 1; char *tb_c825r = tb_mem(tb_c825_0n + tb_c825_1n + 1), *tb_c825rw = tb_c825r; memcpy(tb_c825rw, tb_c825_0, tb_c825_0n); tb_c825rw += tb_c825_0n; memcpy(tb_c825rw, tb_c825_1, tb_c825_1n); tb_c825rw += tb_c825_1n; *tb_c825rw = 0; (const char *)tb_c825r; }));
        if (((tb_o_Node *)tb_nn(v_n, 627))->q_is_const) {
            f_emit__put(v_c, "const ");
        }
        f_emit__put(v_c, ({ const char *tb_c826_0 = "v_"; long long tb_c826_0n = sizeof("v_") - 1; const char *tb_c826_1 = ((tb_o_Node *)tb_nn(v_n, 629))->q_name; long long tb_c826_1n = (long long)strlen(tb_c826_1); const char *tb_c826_2 = " = "; long long tb_c826_2n = sizeof(" = ") - 1; char *tb_c826r = tb_mem(tb_c826_0n + tb_c826_1n + tb_c826_2n + 1), *tb_c826rw = tb_c826r; memcpy(tb_c826rw, tb_c826_0, tb_c826_0n); tb_c826rw += tb_c826_0n; memcpy(tb_c826rw, tb_c826_1, tb_c826_1n); tb_c826rw += tb_c826_1n; memcpy(tb_c826rw, tb_c826_2, tb_c826_2n); tb_c826rw += tb_c826_2n; *tb_c826rw = 0; (const char *)tb_c826r; }));
    } else if (((tb_o_Node *)tb_nn(v_n, 630))->q_is_link) {
        f_emit__put(v_c, ({ const char *tb_c827_0 = "(*v_"; long long tb_c827_0n = sizeof("(*v_") - 1; const char *tb_c827_1 = ((tb_o_Node *)tb_nn(v_n, 631))->q_name; long long tb_c827_1n = (long long)strlen(tb_c827_1); const char *tb_c827_2 = ") = "; long long tb_c827_2n = sizeof(") = ") - 1; char *tb_c827r = tb_mem(tb_c827_0n + tb_c827_1n + tb_c827_2n + 1), *tb_c827rw = tb_c827r; memcpy(tb_c827rw, tb_c827_0, tb_c827_0n); tb_c827rw += tb_c827_0n; memcpy(tb_c827rw, tb_c827_1, tb_c827_1n); tb_c827rw += tb_c827_1n; memcpy(tb_c827rw, tb_c827_2, tb_c827_2n); tb_c827rw += tb_c827_2n; *tb_c827rw = 0; (const char *)tb_c827r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c828_0 = "v_"; long long tb_c828_0n = sizeof("v_") - 1; const char *tb_c828_1 = ((tb_o_Node *)tb_nn(v_n, 633))->q_name; long long tb_c828_1n = (long long)strlen(tb_c828_1); const char *tb_c828_2 = " = "; long long tb_c828_2n = sizeof(" = ") - 1; char *tb_c828r = tb_mem(tb_c828_0n + tb_c828_1n + tb_c828_2n + 1), *tb_c828rw = tb_c828r; memcpy(tb_c828rw, tb_c828_0, tb_c828_0n); tb_c828rw += tb_c828_0n; memcpy(tb_c828rw, tb_c828_1, tb_c828_1n); tb_c828rw += tb_c828_1n; memcpy(tb_c828rw, tb_c828_2, tb_c828_2n); tb_c828rw += tb_c828_2n; *tb_c828rw = 0; (const char *)tb_c828r; }));
    }
    if (((((tb_o_Node *)tb_nn(v_n, 634))->q_kids)->len == 1LL)) {
        f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 635))->q_kids, 0LL, sizeof(tb_o_Node *), 635)), ((tb_o_Node *)tb_nn(v_n, 635))->q_ty);
    } else {
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_n, 637))->q_ty);
    }
    f_emit__put(v_c, ";");
    if (v_bld) {
        f_emit__put(v_c, ({ const char *tb_c829_0 = " tb_rl_"; long long tb_c829_0n = sizeof(" tb_rl_") - 1; const char *tb_c829_1 = ((tb_o_Node *)tb_nn(v_n, 641))->q_name; long long tb_c829_1n = (long long)strlen(tb_c829_1); const char *tb_c829_2 = " = -1;"; long long tb_c829_2n = sizeof(" = -1;") - 1; char *tb_c829r = tb_mem(tb_c829_0n + tb_c829_1n + tb_c829_2n + 1), *tb_c829rw = tb_c829r; memcpy(tb_c829rw, tb_c829_0, tb_c829_0n); tb_c829rw += tb_c829_0n; memcpy(tb_c829rw, tb_c829_1, tb_c829_1n); tb_c829rw += tb_c829_1n; memcpy(tb_c829rw, tb_c829_2, tb_c829_2n); tb_c829rw += tb_c829_2n; *tb_c829rw = 0; (const char *)tb_c829r; }));
    }
    f_emit__put(v_c, "\n");
}

static void f_emit__emit_set(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 646))->q_kids, 0LL, sizeof(tb_o_Node *), 646));
    tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 647))->q_kids, 1LL, sizeof(tb_o_Node *), 647));
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 648))->q_value;
    const char * v_t = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 649))->q_ty);
    f_emit__emit_pre(v_c, v_value, v_depth);
    { long long tb_v830 = 1LL; long long *tb_p830 = &(((tb_o_Comp *)tb_nn(v_c, 651))->q_label); *tb_p830 += tb_v830; }
    const char * v_id = ({ const char *tb_c831_0 = ""; long long tb_c831_0n = sizeof("") - 1; char tb_c831_1d[24]; long long tb_c831_1n; const char *tb_c831_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 652))->q_label, tb_c831_1d, &tb_c831_1n); char *tb_c831r = tb_mem(tb_c831_0n + tb_c831_1n + 1), *tb_c831rw = tb_c831r; memcpy(tb_c831rw, tb_c831_0, tb_c831_0n); tb_c831rw += tb_c831_0n; memcpy(tb_c831rw, tb_c831_1, tb_c831_1n); tb_c831rw += tb_c831_1n; *tb_c831rw = 0; (const char *)tb_c831r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{ ");
    if ((({ const char *tb_s832 = ((tb_o_Node *)tb_nn(v_target, 655))->q_kind; ((unsigned char)tb_s832[0] == 105 && (unsigned char)tb_s832[1] == 110 && (unsigned char)tb_s832[2] == 100 && (unsigned char)tb_s832[3] == 101 && (unsigned char)tb_s832[4] == 120 && tb_s832[5] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 655))->q_kids, 0LL, sizeof(tb_o_Node *), 655)), 655))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c833_0 = v_t; long long tb_c833_0n = (long long)strlen(tb_c833_0); const char *tb_c833_1 = " tb_v"; long long tb_c833_1n = sizeof(" tb_v") - 1; const char *tb_c833_2 = v_id; long long tb_c833_2n = (long long)strlen(tb_c833_2); const char *tb_c833_3 = " = "; long long tb_c833_3n = sizeof(" = ") - 1; char *tb_c833r = tb_mem(tb_c833_0n + tb_c833_1n + tb_c833_2n + tb_c833_3n + 1), *tb_c833rw = tb_c833r; memcpy(tb_c833rw, tb_c833_0, tb_c833_0n); tb_c833rw += tb_c833_0n; memcpy(tb_c833rw, tb_c833_1, tb_c833_1n); tb_c833rw += tb_c833_1n; memcpy(tb_c833rw, tb_c833_2, tb_c833_2n); tb_c833rw += tb_c833_2n; memcpy(tb_c833rw, tb_c833_3, tb_c833_3n); tb_c833rw += tb_c833_3n; *tb_c833rw = 0; (const char *)tb_c833r; }));
        f_emit__emit_expr(v_c, v_value);
        f_emit__put(v_c, ({ const char *tb_c834_0 = "; tb_colset_"; long long tb_c834_0n = sizeof("; tb_colset_") - 1; const char *tb_c834_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 659))->q_kids, 0LL, sizeof(tb_o_Node *), 659)), 659))->q_ty); long long tb_c834_1n = (long long)strlen(tb_c834_1); const char *tb_c834_2 = "("; long long tb_c834_2n = sizeof("(") - 1; char *tb_c834r = tb_mem(tb_c834_0n + tb_c834_1n + tb_c834_2n + 1), *tb_c834rw = tb_c834r; memcpy(tb_c834rw, tb_c834_0, tb_c834_0n); tb_c834rw += tb_c834_0n; memcpy(tb_c834rw, tb_c834_1, tb_c834_1n); tb_c834rw += tb_c834_1n; memcpy(tb_c834rw, tb_c834_2, tb_c834_2n); tb_c834rw += tb_c834_2n; *tb_c834rw = 0; (const char *)tb_c834r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 660))->q_kids, 0LL, sizeof(tb_o_Node *), 660)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 662))->q_kids, 1LL, sizeof(tb_o_Node *), 662)));
        f_emit__put(v_c, ({ const char *tb_c835_0 = ", tb_v"; long long tb_c835_0n = sizeof(", tb_v") - 1; const char *tb_c835_1 = v_id; long long tb_c835_1n = (long long)strlen(tb_c835_1); const char *tb_c835_2 = ", "; long long tb_c835_2n = sizeof(", ") - 1; char tb_c835_3d[24]; long long tb_c835_3n; const char *tb_c835_3 = tb_digits(((tb_o_Node *)tb_nn(v_n, 663))->q_line, tb_c835_3d, &tb_c835_3n); const char *tb_c835_4 = "); }\n"; long long tb_c835_4n = sizeof("); }\n") - 1; char *tb_c835r = tb_mem(tb_c835_0n + tb_c835_1n + tb_c835_2n + tb_c835_3n + tb_c835_4n + 1), *tb_c835rw = tb_c835r; memcpy(tb_c835rw, tb_c835_0, tb_c835_0n); tb_c835rw += tb_c835_0n; memcpy(tb_c835rw, tb_c835_1, tb_c835_1n); tb_c835rw += tb_c835_1n; memcpy(tb_c835rw, tb_c835_2, tb_c835_2n); tb_c835rw += tb_c835_2n; memcpy(tb_c835rw, tb_c835_3, tb_c835_3n); tb_c835rw += tb_c835_3n; memcpy(tb_c835rw, tb_c835_4, tb_c835_4n); tb_c835rw += tb_c835_4n; *tb_c835rw = 0; (const char *)tb_c835r; }));
        return;
    }
    if (((({ const char *tb_s836 = ((tb_o_Node *)tb_nn(v_target, 665))->q_kind; ((unsigned char)tb_s836[0] == 105 && (unsigned char)tb_s836[1] == 110 && (unsigned char)tb_s836[2] == 100 && (unsigned char)tb_s836[3] == 101 && (unsigned char)tb_s836[4] == 120 && tb_s836[5] == 0); }) && f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 665))->q_kids, 0LL, sizeof(tb_o_Node *), 665)), 665))->q_ty)) && ({ const char *tb_s837 = v_op; ((unsigned char)tb_s837[0] == 61 && tb_s837[1] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c838_0 = "_Bool tb_v"; long long tb_c838_0n = sizeof("_Bool tb_v") - 1; const char *tb_c838_1 = v_id; long long tb_c838_1n = (long long)strlen(tb_c838_1); const char *tb_c838_2 = " = "; long long tb_c838_2n = sizeof(" = ") - 1; char *tb_c838r = tb_mem(tb_c838_0n + tb_c838_1n + tb_c838_2n + 1), *tb_c838rw = tb_c838r; memcpy(tb_c838rw, tb_c838_0, tb_c838_0n); tb_c838rw += tb_c838_0n; memcpy(tb_c838rw, tb_c838_1, tb_c838_1n); tb_c838rw += tb_c838_1n; memcpy(tb_c838rw, tb_c838_2, tb_c838_2n); tb_c838rw += tb_c838_2n; *tb_c838rw = 0; (const char *)tb_c838r; }));
        f_emit__emit_expr(v_c, v_value);
        f_emit__put(v_c, "; tb_bset(");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 670))->q_kids, 0LL, sizeof(tb_o_Node *), 670)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 672))->q_kids, 1LL, sizeof(tb_o_Node *), 672)));
        f_emit__put(v_c, ({ const char *tb_c839_0 = ", tb_v"; long long tb_c839_0n = sizeof(", tb_v") - 1; const char *tb_c839_1 = v_id; long long tb_c839_1n = (long long)strlen(tb_c839_1); const char *tb_c839_2 = ", "; long long tb_c839_2n = sizeof(", ") - 1; char tb_c839_3d[24]; long long tb_c839_3n; const char *tb_c839_3 = tb_digits(((tb_o_Node *)tb_nn(v_n, 673))->q_line, tb_c839_3d, &tb_c839_3n); const char *tb_c839_4 = "); }\n"; long long tb_c839_4n = sizeof("); }\n") - 1; char *tb_c839r = tb_mem(tb_c839_0n + tb_c839_1n + tb_c839_2n + tb_c839_3n + tb_c839_4n + 1), *tb_c839rw = tb_c839r; memcpy(tb_c839rw, tb_c839_0, tb_c839_0n); tb_c839rw += tb_c839_0n; memcpy(tb_c839rw, tb_c839_1, tb_c839_1n); tb_c839rw += tb_c839_1n; memcpy(tb_c839rw, tb_c839_2, tb_c839_2n); tb_c839rw += tb_c839_2n; memcpy(tb_c839rw, tb_c839_3, tb_c839_3n); tb_c839rw += tb_c839_3n; memcpy(tb_c839rw, tb_c839_4, tb_c839_4n); tb_c839rw += tb_c839_4n; *tb_c839rw = 0; (const char *)tb_c839r; }));
        return;
    }
    if ((((({ const char *tb_s840 = v_op; ((unsigned char)tb_s840[0] == 43 && (unsigned char)tb_s840[1] == 61 && tb_s840[2] == 0); }) && f_emit__is_bld(v_c, v_target)) && f_emit__is_text_plus(v_c, v_value)) && (!f_emit__bld_uses(v_value, ((tb_o_Node *)tb_nn(v_target, 675))->q_name)))) {
        f_emit__put(v_c, "} ");
        tb_o_Node * v_r = v_value;
        while (f_emit__is_text_plus(v_c, v_r)) {
            v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 680))->q_kids, 0LL, sizeof(tb_o_Node *), 680));
        }
        f_emit__emit_app(v_c, ((tb_o_Node *)tb_nn(v_target, 681))->q_name, v_r);
        {
            tb_list * tb_l841 = f_emit__app_parts(v_c, v_value);
            for (long long tb_i841 = 0; tb_i841 < tb_l841->len; tb_i841++) {
                tb_o_Node * const v_part = ((tb_o_Node * *)tb_l841->data)[tb_i841];
                f_emit__put(v_c, " ");
                f_emit__emit_app(v_c, ((tb_o_Node *)tb_nn(v_target, 684))->q_name, v_part);
            }
        }
        f_emit__put(v_c, "\n");
        return;
    }
    if ((({ const char *tb_s842 = v_op; ((unsigned char)tb_s842[0] == 43 && (unsigned char)tb_s842[1] == 61 && tb_s842[2] == 0); }) && f_emit__is_bld(v_c, v_target))) {
        f_emit__put(v_c, ({ const char *tb_c843_0 = "const char *tb_v"; long long tb_c843_0n = sizeof("const char *tb_v") - 1; const char *tb_c843_1 = v_id; long long tb_c843_1n = (long long)strlen(tb_c843_1); const char *tb_c843_2 = " = "; long long tb_c843_2n = sizeof(" = ") - 1; char *tb_c843r = tb_mem(tb_c843_0n + tb_c843_1n + tb_c843_2n + 1), *tb_c843rw = tb_c843r; memcpy(tb_c843rw, tb_c843_0, tb_c843_0n); tb_c843rw += tb_c843_0n; memcpy(tb_c843rw, tb_c843_1, tb_c843_1n); tb_c843rw += tb_c843_1n; memcpy(tb_c843rw, tb_c843_2, tb_c843_2n); tb_c843rw += tb_c843_2n; *tb_c843rw = 0; (const char *)tb_c843r; }));
        f_emit__emit_as_text(v_c, v_value);
        f_emit__put(v_c, ({ const char *tb_c844_0 = "; tb_app(&v_"; long long tb_c844_0n = sizeof("; tb_app(&v_") - 1; const char *tb_c844_1 = ((tb_o_Node *)tb_nn(v_target, 691))->q_name; long long tb_c844_1n = (long long)strlen(tb_c844_1); const char *tb_c844_2 = ", &tb_rl_"; long long tb_c844_2n = sizeof(", &tb_rl_") - 1; const char *tb_c844_3 = ((tb_o_Node *)tb_nn(v_target, 691))->q_name; long long tb_c844_3n = (long long)strlen(tb_c844_3); const char *tb_c844_4 = ", &tb_rc_"; long long tb_c844_4n = sizeof(", &tb_rc_") - 1; const char *tb_c844_5 = ((tb_o_Node *)tb_nn(v_target, 691))->q_name; long long tb_c844_5n = (long long)strlen(tb_c844_5); const char *tb_c844_6 = ", tb_v"; long long tb_c844_6n = sizeof(", tb_v") - 1; const char *tb_c844_7 = v_id; long long tb_c844_7n = (long long)strlen(tb_c844_7); const char *tb_c844_8 = "); }\n"; long long tb_c844_8n = sizeof("); }\n") - 1; char *tb_c844r = tb_mem(tb_c844_0n + tb_c844_1n + tb_c844_2n + tb_c844_3n + tb_c844_4n + tb_c844_5n + tb_c844_6n + tb_c844_7n + tb_c844_8n + 1), *tb_c844rw = tb_c844r; memcpy(tb_c844rw, tb_c844_0, tb_c844_0n); tb_c844rw += tb_c844_0n; memcpy(tb_c844rw, tb_c844_1, tb_c844_1n); tb_c844rw += tb_c844_1n; memcpy(tb_c844rw, tb_c844_2, tb_c844_2n); tb_c844rw += tb_c844_2n; memcpy(tb_c844rw, tb_c844_3, tb_c844_3n); tb_c844rw += tb_c844_3n; memcpy(tb_c844rw, tb_c844_4, tb_c844_4n); tb_c844rw += tb_c844_4n; memcpy(tb_c844rw, tb_c844_5, tb_c844_5n); tb_c844rw += tb_c844_5n; memcpy(tb_c844rw, tb_c844_6, tb_c844_6n); tb_c844rw += tb_c844_6n; memcpy(tb_c844rw, tb_c844_7, tb_c844_7n); tb_c844rw += tb_c844_7n; memcpy(tb_c844rw, tb_c844_8, tb_c844_8n); tb_c844rw += tb_c844_8n; *tb_c844rw = 0; (const char *)tb_c844r; }));
        return;
    }
    if ((({ const char *tb_s845 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 693))->q_ty); ((unsigned char)tb_s845[0] == 116 && (unsigned char)tb_s845[1] == 101 && (unsigned char)tb_s845[2] == 120 && (unsigned char)tb_s845[3] == 116 && tb_s845[4] == 0); }) && ({ const char *tb_s846 = v_op; !((unsigned char)tb_s846[0] == 61 && tb_s846[1] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c847_0 = "const char *tb_v"; long long tb_c847_0n = sizeof("const char *tb_v") - 1; const char *tb_c847_1 = v_id; long long tb_c847_1n = (long long)strlen(tb_c847_1); const char *tb_c847_2 = " = "; long long tb_c847_2n = sizeof(" = ") - 1; char *tb_c847r = tb_mem(tb_c847_0n + tb_c847_1n + tb_c847_2n + 1), *tb_c847rw = tb_c847r; memcpy(tb_c847rw, tb_c847_0, tb_c847_0n); tb_c847rw += tb_c847_0n; memcpy(tb_c847rw, tb_c847_1, tb_c847_1n); tb_c847rw += tb_c847_1n; memcpy(tb_c847rw, tb_c847_2, tb_c847_2n); tb_c847rw += tb_c847_2n; *tb_c847rw = 0; (const char *)tb_c847r; }));
        f_emit__emit_as_text(v_c, v_value);
    } else {
        if (({ const char *tb_s848 = v_op; ((unsigned char)tb_s848[0] == 61 && tb_s848[1] == 0); })) {
            f_emit__put(v_c, ({ const char *tb_c849_0 = v_t; long long tb_c849_0n = (long long)strlen(tb_c849_0); const char *tb_c849_1 = " tb_v"; long long tb_c849_1n = sizeof(" tb_v") - 1; const char *tb_c849_2 = v_id; long long tb_c849_2n = (long long)strlen(tb_c849_2); const char *tb_c849_3 = " = "; long long tb_c849_3n = sizeof(" = ") - 1; char *tb_c849r = tb_mem(tb_c849_0n + tb_c849_1n + tb_c849_2n + tb_c849_3n + 1), *tb_c849rw = tb_c849r; memcpy(tb_c849rw, tb_c849_0, tb_c849_0n); tb_c849rw += tb_c849_0n; memcpy(tb_c849rw, tb_c849_1, tb_c849_1n); tb_c849rw += tb_c849_1n; memcpy(tb_c849rw, tb_c849_2, tb_c849_2n); tb_c849rw += tb_c849_2n; memcpy(tb_c849rw, tb_c849_3, tb_c849_3n); tb_c849rw += tb_c849_3n; *tb_c849rw = 0; (const char *)tb_c849r; }));
            f_emit__emit_as(v_c, v_value, ((tb_o_Node *)tb_nn(v_n, 699))->q_ty);
        } else {
            f_emit__put(v_c, ({ const char *tb_c850_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_value, 701))->q_ty); long long tb_c850_0n = (long long)strlen(tb_c850_0); const char *tb_c850_1 = " tb_v"; long long tb_c850_1n = sizeof(" tb_v") - 1; const char *tb_c850_2 = v_id; long long tb_c850_2n = (long long)strlen(tb_c850_2); const char *tb_c850_3 = " = "; long long tb_c850_3n = sizeof(" = ") - 1; char *tb_c850r = tb_mem(tb_c850_0n + tb_c850_1n + tb_c850_2n + tb_c850_3n + 1), *tb_c850rw = tb_c850r; memcpy(tb_c850rw, tb_c850_0, tb_c850_0n); tb_c850rw += tb_c850_0n; memcpy(tb_c850rw, tb_c850_1, tb_c850_1n); tb_c850rw += tb_c850_1n; memcpy(tb_c850rw, tb_c850_2, tb_c850_2n); tb_c850rw += tb_c850_2n; memcpy(tb_c850rw, tb_c850_3, tb_c850_3n); tb_c850rw += tb_c850_3n; *tb_c850rw = 0; (const char *)tb_c850r; }));
            f_emit__emit_expr(v_c, v_value);
        }
    }
    f_emit__put(v_c, ({ const char *tb_c851_0 = "; "; long long tb_c851_0n = sizeof("; ") - 1; const char *tb_c851_1 = v_t; long long tb_c851_1n = (long long)strlen(tb_c851_1); const char *tb_c851_2 = " *tb_p"; long long tb_c851_2n = sizeof(" *tb_p") - 1; const char *tb_c851_3 = v_id; long long tb_c851_3n = (long long)strlen(tb_c851_3); const char *tb_c851_4 = " = &("; long long tb_c851_4n = sizeof(" = &(") - 1; char *tb_c851r = tb_mem(tb_c851_0n + tb_c851_1n + tb_c851_2n + tb_c851_3n + tb_c851_4n + 1), *tb_c851rw = tb_c851r; memcpy(tb_c851rw, tb_c851_0, tb_c851_0n); tb_c851rw += tb_c851_0n; memcpy(tb_c851rw, tb_c851_1, tb_c851_1n); tb_c851rw += tb_c851_1n; memcpy(tb_c851rw, tb_c851_2, tb_c851_2n); tb_c851rw += tb_c851_2n; memcpy(tb_c851rw, tb_c851_3, tb_c851_3n); tb_c851rw += tb_c851_3n; memcpy(tb_c851rw, tb_c851_4, tb_c851_4n); tb_c851rw += tb_c851_4n; *tb_c851rw = 0; (const char *)tb_c851r; }));
    f_emit__emit_target(v_c, v_target);
    f_emit__put(v_c, "); ");
    if (({ const char *tb_s852 = v_op; ((unsigned char)tb_s852[0] == 61 && tb_s852[1] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c853_0 = "*tb_p"; long long tb_c853_0n = sizeof("*tb_p") - 1; const char *tb_c853_1 = v_id; long long tb_c853_1n = (long long)strlen(tb_c853_1); const char *tb_c853_2 = " = tb_v"; long long tb_c853_2n = sizeof(" = tb_v") - 1; const char *tb_c853_3 = v_id; long long tb_c853_3n = (long long)strlen(tb_c853_3); const char *tb_c853_4 = ";"; long long tb_c853_4n = sizeof(";") - 1; char *tb_c853r = tb_mem(tb_c853_0n + tb_c853_1n + tb_c853_2n + tb_c853_3n + tb_c853_4n + 1), *tb_c853rw = tb_c853r; memcpy(tb_c853rw, tb_c853_0, tb_c853_0n); tb_c853rw += tb_c853_0n; memcpy(tb_c853rw, tb_c853_1, tb_c853_1n); tb_c853rw += tb_c853_1n; memcpy(tb_c853rw, tb_c853_2, tb_c853_2n); tb_c853rw += tb_c853_2n; memcpy(tb_c853rw, tb_c853_3, tb_c853_3n); tb_c853rw += tb_c853_3n; memcpy(tb_c853rw, tb_c853_4, tb_c853_4n); tb_c853rw += tb_c853_4n; *tb_c853rw = 0; (const char *)tb_c853r; }));
    } else if (({ const char *tb_s854 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 708))->q_ty); ((unsigned char)tb_s854[0] == 116 && (unsigned char)tb_s854[1] == 101 && (unsigned char)tb_s854[2] == 120 && (unsigned char)tb_s854[3] == 116 && tb_s854[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c855_0 = "*tb_p"; long long tb_c855_0n = sizeof("*tb_p") - 1; const char *tb_c855_1 = v_id; long long tb_c855_1n = (long long)strlen(tb_c855_1); const char *tb_c855_2 = " = tb_cat(*tb_p"; long long tb_c855_2n = sizeof(" = tb_cat(*tb_p") - 1; const char *tb_c855_3 = v_id; long long tb_c855_3n = (long long)strlen(tb_c855_3); const char *tb_c855_4 = ", tb_v"; long long tb_c855_4n = sizeof(", tb_v") - 1; const char *tb_c855_5 = v_id; long long tb_c855_5n = (long long)strlen(tb_c855_5); const char *tb_c855_6 = ");"; long long tb_c855_6n = sizeof(");") - 1; char *tb_c855r = tb_mem(tb_c855_0n + tb_c855_1n + tb_c855_2n + tb_c855_3n + tb_c855_4n + tb_c855_5n + tb_c855_6n + 1), *tb_c855rw = tb_c855r; memcpy(tb_c855rw, tb_c855_0, tb_c855_0n); tb_c855rw += tb_c855_0n; memcpy(tb_c855rw, tb_c855_1, tb_c855_1n); tb_c855rw += tb_c855_1n; memcpy(tb_c855rw, tb_c855_2, tb_c855_2n); tb_c855rw += tb_c855_2n; memcpy(tb_c855rw, tb_c855_3, tb_c855_3n); tb_c855rw += tb_c855_3n; memcpy(tb_c855rw, tb_c855_4, tb_c855_4n); tb_c855rw += tb_c855_4n; memcpy(tb_c855rw, tb_c855_5, tb_c855_5n); tb_c855rw += tb_c855_5n; memcpy(tb_c855rw, tb_c855_6, tb_c855_6n); tb_c855rw += tb_c855_6n; *tb_c855rw = 0; (const char *)tb_c855r; }));
    } else if ((({ const char *tb_s856 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 710))->q_ty); ((unsigned char)tb_s856[0] == 105 && (unsigned char)tb_s856[1] == 110 && (unsigned char)tb_s856[2] == 116 && tb_s856[3] == 0); }) && ({ const char *tb_s857 = v_op; ((unsigned char)tb_s857[0] == 47 && (unsigned char)tb_s857[1] == 61 && tb_s857[2] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c858_0 = "*tb_p"; long long tb_c858_0n = sizeof("*tb_p") - 1; const char *tb_c858_1 = v_id; long long tb_c858_1n = (long long)strlen(tb_c858_1); const char *tb_c858_2 = " = tb_div(*tb_p"; long long tb_c858_2n = sizeof(" = tb_div(*tb_p") - 1; const char *tb_c858_3 = v_id; long long tb_c858_3n = (long long)strlen(tb_c858_3); const char *tb_c858_4 = ", tb_v"; long long tb_c858_4n = sizeof(", tb_v") - 1; const char *tb_c858_5 = v_id; long long tb_c858_5n = (long long)strlen(tb_c858_5); const char *tb_c858_6 = ", "; long long tb_c858_6n = sizeof(", ") - 1; char tb_c858_7d[24]; long long tb_c858_7n; const char *tb_c858_7 = tb_digits(((tb_o_Node *)tb_nn(v_n, 711))->q_line, tb_c858_7d, &tb_c858_7n); const char *tb_c858_8 = ");"; long long tb_c858_8n = sizeof(");") - 1; char *tb_c858r = tb_mem(tb_c858_0n + tb_c858_1n + tb_c858_2n + tb_c858_3n + tb_c858_4n + tb_c858_5n + tb_c858_6n + tb_c858_7n + tb_c858_8n + 1), *tb_c858rw = tb_c858r; memcpy(tb_c858rw, tb_c858_0, tb_c858_0n); tb_c858rw += tb_c858_0n; memcpy(tb_c858rw, tb_c858_1, tb_c858_1n); tb_c858rw += tb_c858_1n; memcpy(tb_c858rw, tb_c858_2, tb_c858_2n); tb_c858rw += tb_c858_2n; memcpy(tb_c858rw, tb_c858_3, tb_c858_3n); tb_c858rw += tb_c858_3n; memcpy(tb_c858rw, tb_c858_4, tb_c858_4n); tb_c858rw += tb_c858_4n; memcpy(tb_c858rw, tb_c858_5, tb_c858_5n); tb_c858rw += tb_c858_5n; memcpy(tb_c858rw, tb_c858_6, tb_c858_6n); tb_c858rw += tb_c858_6n; memcpy(tb_c858rw, tb_c858_7, tb_c858_7n); tb_c858rw += tb_c858_7n; memcpy(tb_c858rw, tb_c858_8, tb_c858_8n); tb_c858rw += tb_c858_8n; *tb_c858rw = 0; (const char *)tb_c858r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c859_0 = "*tb_p"; long long tb_c859_0n = sizeof("*tb_p") - 1; const char *tb_c859_1 = v_id; long long tb_c859_1n = (long long)strlen(tb_c859_1); const char *tb_c859_2 = " "; long long tb_c859_2n = sizeof(" ") - 1; const char *tb_c859_3 = v_op; long long tb_c859_3n = (long long)strlen(tb_c859_3); const char *tb_c859_4 = " tb_v"; long long tb_c859_4n = sizeof(" tb_v") - 1; const char *tb_c859_5 = v_id; long long tb_c859_5n = (long long)strlen(tb_c859_5); const char *tb_c859_6 = ";"; long long tb_c859_6n = sizeof(";") - 1; char *tb_c859r = tb_mem(tb_c859_0n + tb_c859_1n + tb_c859_2n + tb_c859_3n + tb_c859_4n + tb_c859_5n + tb_c859_6n + 1), *tb_c859rw = tb_c859r; memcpy(tb_c859rw, tb_c859_0, tb_c859_0n); tb_c859rw += tb_c859_0n; memcpy(tb_c859rw, tb_c859_1, tb_c859_1n); tb_c859rw += tb_c859_1n; memcpy(tb_c859rw, tb_c859_2, tb_c859_2n); tb_c859rw += tb_c859_2n; memcpy(tb_c859rw, tb_c859_3, tb_c859_3n); tb_c859rw += tb_c859_3n; memcpy(tb_c859rw, tb_c859_4, tb_c859_4n); tb_c859rw += tb_c859_4n; memcpy(tb_c859rw, tb_c859_5, tb_c859_5n); tb_c859rw += tb_c859_5n; memcpy(tb_c859rw, tb_c859_6, tb_c859_6n); tb_c859rw += tb_c859_6n; *tb_c859rw = 0; (const char *)tb_c859r; }));
    }
    f_emit__put(v_c, " }\n");
}

static void f_emit__emit_multi(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    long long v_names = ((((tb_o_Node *)tb_nn(v_n, 717))->q_kids)->len - 1LL);
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 718))->q_kids, v_names, sizeof(tb_o_Node *), 718));
    f_emit__emit_pre(v_c, v_call, v_depth);
    { long long tb_v860 = 1LL; long long *tb_p860 = &(((tb_o_Comp *)tb_nn(v_c, 720))->q_label); *tb_p860 += tb_v860; }
    const char * v_id = ({ const char *tb_c861_0 = ""; long long tb_c861_0n = sizeof("") - 1; char tb_c861_1d[24]; long long tb_c861_1n; const char *tb_c861_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 721))->q_label, tb_c861_1d, &tb_c861_1n); char *tb_c861r = tb_mem(tb_c861_0n + tb_c861_1n + 1), *tb_c861rw = tb_c861r; memcpy(tb_c861rw, tb_c861_0, tb_c861_0n); tb_c861rw += tb_c861_0n; memcpy(tb_c861rw, tb_c861_1, tb_c861_1n); tb_c861rw += tb_c861_1n; *tb_c861rw = 0; (const char *)tb_c861r; });
    {
        long long tb_end862 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end862; v_i++) {
            if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 723))->q_kids, v_i, sizeof(tb_o_Node *), 723)), 723))->q_creates) {
                f_emit__indent(v_c, v_depth);
                f_emit__put(v_c, ({ const char *tb_c863_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 725))->q_kids, v_i, sizeof(tb_o_Node *), 725)), 725))->q_ty); long long tb_c863_0n = (long long)strlen(tb_c863_0); const char *tb_c863_1 = " v_"; long long tb_c863_1n = sizeof(" v_") - 1; const char *tb_c863_2 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 725))->q_kids, v_i, sizeof(tb_o_Node *), 725)), 725))->q_name; long long tb_c863_2n = (long long)strlen(tb_c863_2); const char *tb_c863_3 = ";\n"; long long tb_c863_3n = sizeof(";\n") - 1; char *tb_c863r = tb_mem(tb_c863_0n + tb_c863_1n + tb_c863_2n + tb_c863_3n + 1), *tb_c863rw = tb_c863r; memcpy(tb_c863rw, tb_c863_0, tb_c863_0n); tb_c863rw += tb_c863_0n; memcpy(tb_c863rw, tb_c863_1, tb_c863_1n); tb_c863rw += tb_c863_1n; memcpy(tb_c863rw, tb_c863_2, tb_c863_2n); tb_c863rw += tb_c863_2n; memcpy(tb_c863rw, tb_c863_3, tb_c863_3n); tb_c863rw += tb_c863_3n; *tb_c863rw = 0; (const char *)tb_c863r; }));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c864_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_call, 729))->q_ty); long long tb_c864_0n = (long long)strlen(tb_c864_0); const char *tb_c864_1 = " tb_m"; long long tb_c864_1n = sizeof(" tb_m") - 1; const char *tb_c864_2 = v_id; long long tb_c864_2n = (long long)strlen(tb_c864_2); const char *tb_c864_3 = " = "; long long tb_c864_3n = sizeof(" = ") - 1; char *tb_c864r = tb_mem(tb_c864_0n + tb_c864_1n + tb_c864_2n + tb_c864_3n + 1), *tb_c864rw = tb_c864r; memcpy(tb_c864rw, tb_c864_0, tb_c864_0n); tb_c864rw += tb_c864_0n; memcpy(tb_c864rw, tb_c864_1, tb_c864_1n); tb_c864rw += tb_c864_1n; memcpy(tb_c864rw, tb_c864_2, tb_c864_2n); tb_c864rw += tb_c864_2n; memcpy(tb_c864rw, tb_c864_3, tb_c864_3n); tb_c864rw += tb_c864_3n; *tb_c864rw = 0; (const char *)tb_c864r; }));
    f_emit__emit_expr(v_c, v_call);
    f_emit__put(v_c, ";\n");
    {
        long long tb_end865 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end865; v_i++) {
            f_emit__indent(v_c, (v_depth + 1LL));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 734))->q_kids, v_i, sizeof(tb_o_Node *), 734)));
            f_emit__put(v_c, ({ const char *tb_c866_0 = " = tb_m"; long long tb_c866_0n = sizeof(" = tb_m") - 1; const char *tb_c866_1 = v_id; long long tb_c866_1n = (long long)strlen(tb_c866_1); const char *tb_c866_2 = ".o"; long long tb_c866_2n = sizeof(".o") - 1; char tb_c866_3d[24]; long long tb_c866_3n; const char *tb_c866_3 = tb_digits(v_i, tb_c866_3d, &tb_c866_3n); const char *tb_c866_4 = ";\n"; long long tb_c866_4n = sizeof(";\n") - 1; char *tb_c866r = tb_mem(tb_c866_0n + tb_c866_1n + tb_c866_2n + tb_c866_3n + tb_c866_4n + 1), *tb_c866rw = tb_c866r; memcpy(tb_c866rw, tb_c866_0, tb_c866_0n); tb_c866rw += tb_c866_0n; memcpy(tb_c866rw, tb_c866_1, tb_c866_1n); tb_c866rw += tb_c866_1n; memcpy(tb_c866rw, tb_c866_2, tb_c866_2n); tb_c866rw += tb_c866_2n; memcpy(tb_c866rw, tb_c866_3, tb_c866_3n); tb_c866rw += tb_c866_3n; memcpy(tb_c866rw, tb_c866_4, tb_c866_4n); tb_c866rw += tb_c866_4n; *tb_c866rw = 0; (const char *)tb_c866r; }));
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_if(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    f_emit__indent(v_c, v_depth);
    long long v_i = 0LL;
    while (((v_i + 1LL) < (((tb_o_Node *)tb_nn(v_n, 742))->q_kids)->len)) {
        if ((v_i > 0LL)) {
            f_emit__put(v_c, " else ");
        }
        f_emit__put(v_c, "if (");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 746))->q_kids, v_i, sizeof(tb_o_Node *), 746)));
        f_emit__put(v_c, ") {\n");
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 748))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 748)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}");
        { long long tb_v867 = 2LL; long long *tb_p867 = &(v_i); *tb_p867 += tb_v867; }
    }
    if ((tb_mod((((tb_o_Node *)tb_nn(v_n, 752))->q_kids)->len, 2LL, 752) == 1LL)) {
        f_emit__put(v_c, " else {\n");
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 754))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 754))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 754)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}");
    }
    f_emit__put(v_c, "\n");
}

static void f_emit__emit_par(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v868 = 1LL; long long *tb_p868 = &(((tb_o_Comp *)tb_nn(v_c, 763))->q_label); *tb_p868 += tb_v868; }
    const char * v_id = ({ const char *tb_c869_0 = ""; long long tb_c869_0n = sizeof("") - 1; char tb_c869_1d[24]; long long tb_c869_1n; const char *tb_c869_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 764))->q_label, tb_c869_1d, &tb_c869_1n); char *tb_c869r = tb_mem(tb_c869_0n + tb_c869_1n + 1), *tb_c869rw = tb_c869r; memcpy(tb_c869rw, tb_c869_0, tb_c869_0n); tb_c869rw += tb_c869_0n; memcpy(tb_c869rw, tb_c869_1, tb_c869_1n); tb_c869rw += tb_c869_1n; *tb_c869rw = 0; (const char *)tb_c869r; });
    _Bool v_is_each = ({ const char *tb_s870 = ((tb_o_Node *)tb_nn(v_n, 765))->q_kind; ((unsigned char)tb_s870[0] == 101 && (unsigned char)tb_s870[1] == 97 && (unsigned char)tb_s870[2] == 99 && (unsigned char)tb_s870[3] == 104 && tb_s870[4] == 0); });
    const char * v_elem = "";
    if (v_is_each) {
        v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 768))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 768))->q_kids, 0LL, sizeof(tb_o_Node *), 768)), 768))->q_ty, sizeof(tb_o_Type *), 768)), 768))->q_elem);
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 770))->q_out;
    tb_list * v_w = tb_list_new(sizeof(const char *));
    { tb_list * tb_v871 = v_w; tb_list * *tb_p871 = &(((tb_o_Comp *)tb_nn(v_c, 772))->q_out); *tb_p871 = tb_v871; }
    const char * v_list_type = "tb_list *";
    if (v_is_each) {
        v_list_type = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 775))->q_kids, 0LL, sizeof(tb_o_Node *), 775)), 775))->q_ty);
    }
    f_emit__put(v_c, ({ const char *tb_c872_0 = "\ntypedef struct {\n    long long tb_lo, tb_total, tb_step, tb_next;\n    "; long long tb_c872_0n = sizeof("\ntypedef struct {\n    long long tb_lo, tb_total, tb_step, tb_next;\n    ") - 1; const char *tb_c872_1 = v_list_type; long long tb_c872_1n = (long long)strlen(tb_c872_1); const char *tb_c872_2 = " tb_l;\n"; long long tb_c872_2n = sizeof(" tb_l;\n") - 1; char *tb_c872r = tb_mem(tb_c872_0n + tb_c872_1n + tb_c872_2n + 1), *tb_c872rw = tb_c872r; memcpy(tb_c872rw, tb_c872_0, tb_c872_0n); tb_c872rw += tb_c872_0n; memcpy(tb_c872rw, tb_c872_1, tb_c872_1n); tb_c872rw += tb_c872_1n; memcpy(tb_c872rw, tb_c872_2, tb_c872_2n); tb_c872rw += tb_c872_2n; *tb_c872rw = 0; (const char *)tb_c872r; }));
    {
        tb_list * tb_l873 = ((tb_o_Node *)tb_nn(v_n, 777))->q_caps;
        for (long long tb_i873 = 0; tb_i873 < tb_l873->len; tb_i873++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l873->data)[tb_i873];
            if ((((tb_o_Var *)tb_nn(v_v, 778))->q_is_shared && ({ const char *tb_s874 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 778))->q_type); ((unsigned char)tb_s874[0] == 116 && (unsigned char)tb_s874[1] == 101 && (unsigned char)tb_s874[2] == 120 && (unsigned char)tb_s874[3] == 116 && tb_s874[4] == 0); }))) {
                f_emit__put(v_c, ({ const char *tb_c875_0 = "    const char *m_"; long long tb_c875_0n = sizeof("    const char *m_") - 1; const char *tb_c875_1 = ((tb_o_Var *)tb_nn(v_v, 780))->q_name; long long tb_c875_1n = (long long)strlen(tb_c875_1); const char *tb_c875_2 = "; long long m_rl_"; long long tb_c875_2n = sizeof("; long long m_rl_") - 1; const char *tb_c875_3 = ((tb_o_Var *)tb_nn(v_v, 780))->q_name; long long tb_c875_3n = (long long)strlen(tb_c875_3); const char *tb_c875_4 = ", m_rc_"; long long tb_c875_4n = sizeof(", m_rc_") - 1; const char *tb_c875_5 = ((tb_o_Var *)tb_nn(v_v, 780))->q_name; long long tb_c875_5n = (long long)strlen(tb_c875_5); const char *tb_c875_6 = ";\n"; long long tb_c875_6n = sizeof(";\n") - 1; char *tb_c875r = tb_mem(tb_c875_0n + tb_c875_1n + tb_c875_2n + tb_c875_3n + tb_c875_4n + tb_c875_5n + tb_c875_6n + 1), *tb_c875rw = tb_c875r; memcpy(tb_c875rw, tb_c875_0, tb_c875_0n); tb_c875rw += tb_c875_0n; memcpy(tb_c875rw, tb_c875_1, tb_c875_1n); tb_c875rw += tb_c875_1n; memcpy(tb_c875rw, tb_c875_2, tb_c875_2n); tb_c875rw += tb_c875_2n; memcpy(tb_c875rw, tb_c875_3, tb_c875_3n); tb_c875rw += tb_c875_3n; memcpy(tb_c875rw, tb_c875_4, tb_c875_4n); tb_c875rw += tb_c875_4n; memcpy(tb_c875rw, tb_c875_5, tb_c875_5n); tb_c875rw += tb_c875_5n; memcpy(tb_c875rw, tb_c875_6, tb_c875_6n); tb_c875rw += tb_c875_6n; *tb_c875rw = 0; (const char *)tb_c875r; }));
            } else if (((tb_o_Var *)tb_nn(v_v, 781))->q_is_shared) {
                f_emit__put(v_c, ({ const char *tb_c876_0 = "    "; long long tb_c876_0n = sizeof("    ") - 1; const char *tb_c876_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 782))->q_type); long long tb_c876_1n = (long long)strlen(tb_c876_1); const char *tb_c876_2 = " *p_"; long long tb_c876_2n = sizeof(" *p_") - 1; const char *tb_c876_3 = ((tb_o_Var *)tb_nn(v_v, 782))->q_name; long long tb_c876_3n = (long long)strlen(tb_c876_3); const char *tb_c876_4 = ";\n"; long long tb_c876_4n = sizeof(";\n") - 1; char *tb_c876r = tb_mem(tb_c876_0n + tb_c876_1n + tb_c876_2n + tb_c876_3n + tb_c876_4n + 1), *tb_c876rw = tb_c876r; memcpy(tb_c876rw, tb_c876_0, tb_c876_0n); tb_c876rw += tb_c876_0n; memcpy(tb_c876rw, tb_c876_1, tb_c876_1n); tb_c876rw += tb_c876_1n; memcpy(tb_c876rw, tb_c876_2, tb_c876_2n); tb_c876rw += tb_c876_2n; memcpy(tb_c876rw, tb_c876_3, tb_c876_3n); tb_c876rw += tb_c876_3n; memcpy(tb_c876rw, tb_c876_4, tb_c876_4n); tb_c876rw += tb_c876_4n; *tb_c876rw = 0; (const char *)tb_c876r; }));
            } else if (((tb_o_Var *)tb_nn(v_v, 783))->q_is_link) {
                f_emit__put(v_c, ({ const char *tb_c877_0 = "    "; long long tb_c877_0n = sizeof("    ") - 1; const char *tb_c877_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 784))->q_type); long long tb_c877_1n = (long long)strlen(tb_c877_1); const char *tb_c877_2 = " *v_"; long long tb_c877_2n = sizeof(" *v_") - 1; const char *tb_c877_3 = ((tb_o_Var *)tb_nn(v_v, 784))->q_name; long long tb_c877_3n = (long long)strlen(tb_c877_3); const char *tb_c877_4 = ";\n"; long long tb_c877_4n = sizeof(";\n") - 1; char *tb_c877r = tb_mem(tb_c877_0n + tb_c877_1n + tb_c877_2n + tb_c877_3n + tb_c877_4n + 1), *tb_c877rw = tb_c877r; memcpy(tb_c877rw, tb_c877_0, tb_c877_0n); tb_c877rw += tb_c877_0n; memcpy(tb_c877rw, tb_c877_1, tb_c877_1n); tb_c877rw += tb_c877_1n; memcpy(tb_c877rw, tb_c877_2, tb_c877_2n); tb_c877rw += tb_c877_2n; memcpy(tb_c877rw, tb_c877_3, tb_c877_3n); tb_c877rw += tb_c877_3n; memcpy(tb_c877rw, tb_c877_4, tb_c877_4n); tb_c877rw += tb_c877_4n; *tb_c877rw = 0; (const char *)tb_c877r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c878_0 = "    "; long long tb_c878_0n = sizeof("    ") - 1; const char *tb_c878_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 786))->q_type); long long tb_c878_1n = (long long)strlen(tb_c878_1); const char *tb_c878_2 = " v_"; long long tb_c878_2n = sizeof(" v_") - 1; const char *tb_c878_3 = ((tb_o_Var *)tb_nn(v_v, 786))->q_name; long long tb_c878_3n = (long long)strlen(tb_c878_3); const char *tb_c878_4 = ";\n"; long long tb_c878_4n = sizeof(";\n") - 1; char *tb_c878r = tb_mem(tb_c878_0n + tb_c878_1n + tb_c878_2n + tb_c878_3n + tb_c878_4n + 1), *tb_c878rw = tb_c878r; memcpy(tb_c878rw, tb_c878_0, tb_c878_0n); tb_c878rw += tb_c878_0n; memcpy(tb_c878rw, tb_c878_1, tb_c878_1n); tb_c878rw += tb_c878_1n; memcpy(tb_c878rw, tb_c878_2, tb_c878_2n); tb_c878rw += tb_c878_2n; memcpy(tb_c878rw, tb_c878_3, tb_c878_3n); tb_c878rw += tb_c878_3n; memcpy(tb_c878rw, tb_c878_4, tb_c878_4n); tb_c878rw += tb_c878_4n; *tb_c878rw = 0; (const char *)tb_c878r; }));
            }
        }
    }
    f_emit__put(v_c, ({ const char *tb_c879_0 = "} tb_job"; long long tb_c879_0n = sizeof("} tb_job") - 1; const char *tb_c879_1 = v_id; long long tb_c879_1n = (long long)strlen(tb_c879_1); const char *tb_c879_2 = ";\n"; long long tb_c879_2n = sizeof(";\n") - 1; char *tb_c879r = tb_mem(tb_c879_0n + tb_c879_1n + tb_c879_2n + 1), *tb_c879rw = tb_c879r; memcpy(tb_c879rw, tb_c879_0, tb_c879_0n); tb_c879rw += tb_c879_0n; memcpy(tb_c879rw, tb_c879_1, tb_c879_1n); tb_c879rw += tb_c879_1n; memcpy(tb_c879rw, tb_c879_2, tb_c879_2n); tb_c879rw += tb_c879_2n; *tb_c879rw = 0; (const char *)tb_c879r; }));
    f_emit__put(v_c, ({ const char *tb_c880_0 = "static void *tb_work"; long long tb_c880_0n = sizeof("static void *tb_work") - 1; const char *tb_c880_1 = v_id; long long tb_c880_1n = (long long)strlen(tb_c880_1); const char *tb_c880_2 = "(void *tb_arg) {\n"; long long tb_c880_2n = sizeof("(void *tb_arg) {\n") - 1; char *tb_c880r = tb_mem(tb_c880_0n + tb_c880_1n + tb_c880_2n + 1), *tb_c880rw = tb_c880r; memcpy(tb_c880rw, tb_c880_0, tb_c880_0n); tb_c880rw += tb_c880_0n; memcpy(tb_c880rw, tb_c880_1, tb_c880_1n); tb_c880rw += tb_c880_1n; memcpy(tb_c880rw, tb_c880_2, tb_c880_2n); tb_c880rw += tb_c880_2n; *tb_c880rw = 0; (const char *)tb_c880r; }));
    f_emit__put(v_c, ({ const char *tb_c881_0 = "    tb_job"; long long tb_c881_0n = sizeof("    tb_job") - 1; const char *tb_c881_1 = v_id; long long tb_c881_1n = (long long)strlen(tb_c881_1); const char *tb_c881_2 = " *tb_j = tb_arg;\n"; long long tb_c881_2n = sizeof(" *tb_j = tb_arg;\n") - 1; char *tb_c881r = tb_mem(tb_c881_0n + tb_c881_1n + tb_c881_2n + 1), *tb_c881rw = tb_c881r; memcpy(tb_c881rw, tb_c881_0, tb_c881_0n); tb_c881rw += tb_c881_0n; memcpy(tb_c881rw, tb_c881_1, tb_c881_1n); tb_c881rw += tb_c881_1n; memcpy(tb_c881rw, tb_c881_2, tb_c881_2n); tb_c881rw += tb_c881_2n; *tb_c881rw = 0; (const char *)tb_c881r; }));
    _Bool v_has_shared = 0;
    tb_list * v_saved_builders = ((tb_o_Comp *)tb_nn(v_c, 792))->q_builders;
    tb_list * v_par_builders = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l882 = ((tb_o_Node *)tb_nn(v_n, 794))->q_caps;
        for (long long tb_i882 = 0; tb_i882 < tb_l882->len; tb_i882++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l882->data)[tb_i882];
            if (((tb_o_Var *)tb_nn(v_v, 795))->q_is_shared) {
                v_has_shared = 1;
                f_emit__put(v_c, ({ const char *tb_c883_0 = "    "; long long tb_c883_0n = sizeof("    ") - 1; const char *tb_c883_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 797))->q_type); long long tb_c883_1n = (long long)strlen(tb_c883_1); const char *tb_c883_2 = " v_"; long long tb_c883_2n = sizeof(" v_") - 1; const char *tb_c883_3 = ((tb_o_Var *)tb_nn(v_v, 797))->q_name; long long tb_c883_3n = (long long)strlen(tb_c883_3); const char *tb_c883_4 = " = "; long long tb_c883_4n = sizeof(" = ") - 1; char *tb_c883r = tb_mem(tb_c883_0n + tb_c883_1n + tb_c883_2n + tb_c883_3n + tb_c883_4n + 1), *tb_c883rw = tb_c883r; memcpy(tb_c883rw, tb_c883_0, tb_c883_0n); tb_c883rw += tb_c883_0n; memcpy(tb_c883rw, tb_c883_1, tb_c883_1n); tb_c883rw += tb_c883_1n; memcpy(tb_c883rw, tb_c883_2, tb_c883_2n); tb_c883rw += tb_c883_2n; memcpy(tb_c883rw, tb_c883_3, tb_c883_3n); tb_c883rw += tb_c883_3n; memcpy(tb_c883rw, tb_c883_4, tb_c883_4n); tb_c883rw += tb_c883_4n; *tb_c883rw = 0; (const char *)tb_c883r; }));
                f_emit__emit_default(v_c, ((tb_o_Var *)tb_nn(v_v, 798))->q_type);
                f_emit__put(v_c, ";\n");
                if (({ const char *tb_s884 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 800))->q_type); ((unsigned char)tb_s884[0] == 116 && (unsigned char)tb_s884[1] == 101 && (unsigned char)tb_s884[2] == 120 && (unsigned char)tb_s884[3] == 116 && tb_s884[4] == 0); })) {
                    { const char * tb_v885 = ((tb_o_Var *)tb_nn(v_v, 801))->q_name; *(const char * *)tb_grow(v_par_builders) = tb_v885; }
                    f_emit__put(v_c, ({ const char *tb_c886_0 = "    long long tb_rl_"; long long tb_c886_0n = sizeof("    long long tb_rl_") - 1; const char *tb_c886_1 = ((tb_o_Var *)tb_nn(v_v, 802))->q_name; long long tb_c886_1n = (long long)strlen(tb_c886_1); const char *tb_c886_2 = " = -1, tb_rc_"; long long tb_c886_2n = sizeof(" = -1, tb_rc_") - 1; const char *tb_c886_3 = ((tb_o_Var *)tb_nn(v_v, 802))->q_name; long long tb_c886_3n = (long long)strlen(tb_c886_3); const char *tb_c886_4 = " = 0;\n"; long long tb_c886_4n = sizeof(" = 0;\n") - 1; char *tb_c886r = tb_mem(tb_c886_0n + tb_c886_1n + tb_c886_2n + tb_c886_3n + tb_c886_4n + 1), *tb_c886rw = tb_c886r; memcpy(tb_c886rw, tb_c886_0, tb_c886_0n); tb_c886rw += tb_c886_0n; memcpy(tb_c886rw, tb_c886_1, tb_c886_1n); tb_c886rw += tb_c886_1n; memcpy(tb_c886rw, tb_c886_2, tb_c886_2n); tb_c886rw += tb_c886_2n; memcpy(tb_c886rw, tb_c886_3, tb_c886_3n); tb_c886rw += tb_c886_3n; memcpy(tb_c886rw, tb_c886_4, tb_c886_4n); tb_c886rw += tb_c886_4n; *tb_c886rw = 0; (const char *)tb_c886r; }));
                }
            } else if (((tb_o_Var *)tb_nn(v_v, 803))->q_is_link) {
                f_emit__put(v_c, ({ const char *tb_c887_0 = "    "; long long tb_c887_0n = sizeof("    ") - 1; const char *tb_c887_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 804))->q_type); long long tb_c887_1n = (long long)strlen(tb_c887_1); const char *tb_c887_2 = " *v_"; long long tb_c887_2n = sizeof(" *v_") - 1; const char *tb_c887_3 = ((tb_o_Var *)tb_nn(v_v, 804))->q_name; long long tb_c887_3n = (long long)strlen(tb_c887_3); const char *tb_c887_4 = " = tb_j->v_"; long long tb_c887_4n = sizeof(" = tb_j->v_") - 1; const char *tb_c887_5 = ((tb_o_Var *)tb_nn(v_v, 804))->q_name; long long tb_c887_5n = (long long)strlen(tb_c887_5); const char *tb_c887_6 = ";\n"; long long tb_c887_6n = sizeof(";\n") - 1; char *tb_c887r = tb_mem(tb_c887_0n + tb_c887_1n + tb_c887_2n + tb_c887_3n + tb_c887_4n + tb_c887_5n + tb_c887_6n + 1), *tb_c887rw = tb_c887r; memcpy(tb_c887rw, tb_c887_0, tb_c887_0n); tb_c887rw += tb_c887_0n; memcpy(tb_c887rw, tb_c887_1, tb_c887_1n); tb_c887rw += tb_c887_1n; memcpy(tb_c887rw, tb_c887_2, tb_c887_2n); tb_c887rw += tb_c887_2n; memcpy(tb_c887rw, tb_c887_3, tb_c887_3n); tb_c887rw += tb_c887_3n; memcpy(tb_c887rw, tb_c887_4, tb_c887_4n); tb_c887rw += tb_c887_4n; memcpy(tb_c887rw, tb_c887_5, tb_c887_5n); tb_c887rw += tb_c887_5n; memcpy(tb_c887rw, tb_c887_6, tb_c887_6n); tb_c887rw += tb_c887_6n; *tb_c887rw = 0; (const char *)tb_c887r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c888_0 = "    "; long long tb_c888_0n = sizeof("    ") - 1; const char *tb_c888_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 806))->q_type); long long tb_c888_1n = (long long)strlen(tb_c888_1); const char *tb_c888_2 = " v_"; long long tb_c888_2n = sizeof(" v_") - 1; const char *tb_c888_3 = ((tb_o_Var *)tb_nn(v_v, 806))->q_name; long long tb_c888_3n = (long long)strlen(tb_c888_3); const char *tb_c888_4 = " = tb_j->v_"; long long tb_c888_4n = sizeof(" = tb_j->v_") - 1; const char *tb_c888_5 = ((tb_o_Var *)tb_nn(v_v, 806))->q_name; long long tb_c888_5n = (long long)strlen(tb_c888_5); const char *tb_c888_6 = ";\n"; long long tb_c888_6n = sizeof(";\n") - 1; char *tb_c888r = tb_mem(tb_c888_0n + tb_c888_1n + tb_c888_2n + tb_c888_3n + tb_c888_4n + tb_c888_5n + tb_c888_6n + 1), *tb_c888rw = tb_c888r; memcpy(tb_c888rw, tb_c888_0, tb_c888_0n); tb_c888rw += tb_c888_0n; memcpy(tb_c888rw, tb_c888_1, tb_c888_1n); tb_c888rw += tb_c888_1n; memcpy(tb_c888rw, tb_c888_2, tb_c888_2n); tb_c888rw += tb_c888_2n; memcpy(tb_c888rw, tb_c888_3, tb_c888_3n); tb_c888rw += tb_c888_3n; memcpy(tb_c888rw, tb_c888_4, tb_c888_4n); tb_c888rw += tb_c888_4n; memcpy(tb_c888rw, tb_c888_5, tb_c888_5n); tb_c888rw += tb_c888_5n; memcpy(tb_c888rw, tb_c888_6, tb_c888_6n); tb_c888rw += tb_c888_6n; *tb_c888rw = 0; (const char *)tb_c888r; }));
            }
        }
    }
    { tb_list * tb_v889 = v_par_builders; tb_list * *tb_p889 = &(((tb_o_Comp *)tb_nn(v_c, 807))->q_builders); *tb_p889 = tb_v889; }
    f_emit__put(v_c, "    for (;;) {\n");
    f_emit__put(v_c, "        long long tb_s = __atomic_fetch_add(&tb_j->tb_next, tb_j->tb_step, __ATOMIC_RELAXED);\n");
    f_emit__put(v_c, "        if (tb_s >= tb_j->tb_total) break;\n");
    f_emit__put(v_c, "        long long tb_e = tb_s + tb_j->tb_step;\n");
    f_emit__put(v_c, "        if (tb_e > tb_j->tb_total) tb_e = tb_j->tb_total;\n");
    f_emit__put(v_c, "        for (long long tb_i = tb_s; tb_i < tb_e; tb_i++) {\n");
    if ((v_is_each && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 814))->q_kids, 0LL, sizeof(tb_o_Node *), 814)), 814))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c890_0 = "            "; long long tb_c890_0n = sizeof("            ") - 1; const char *tb_c890_1 = v_elem; long long tb_c890_1n = (long long)strlen(tb_c890_1); const char *tb_c890_2 = " const v_"; long long tb_c890_2n = sizeof(" const v_") - 1; const char *tb_c890_3 = ((tb_o_Node *)tb_nn(v_n, 815))->q_name; long long tb_c890_3n = (long long)strlen(tb_c890_3); const char *tb_c890_4 = " = {tb_j->tb_l, tb_i};\n"; long long tb_c890_4n = sizeof(" = {tb_j->tb_l, tb_i};\n") - 1; char *tb_c890r = tb_mem(tb_c890_0n + tb_c890_1n + tb_c890_2n + tb_c890_3n + tb_c890_4n + 1), *tb_c890rw = tb_c890r; memcpy(tb_c890rw, tb_c890_0, tb_c890_0n); tb_c890rw += tb_c890_0n; memcpy(tb_c890rw, tb_c890_1, tb_c890_1n); tb_c890rw += tb_c890_1n; memcpy(tb_c890rw, tb_c890_2, tb_c890_2n); tb_c890rw += tb_c890_2n; memcpy(tb_c890rw, tb_c890_3, tb_c890_3n); tb_c890rw += tb_c890_3n; memcpy(tb_c890rw, tb_c890_4, tb_c890_4n); tb_c890rw += tb_c890_4n; *tb_c890rw = 0; (const char *)tb_c890r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 816))->q_kids, 1LL, sizeof(tb_o_Node *), 816)), 3LL);
    } else if ((v_is_each && f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 817))->q_kids, 0LL, sizeof(tb_o_Node *), 817)), 817))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c891_0 = "            _Bool const v_"; long long tb_c891_0n = sizeof("            _Bool const v_") - 1; const char *tb_c891_1 = ((tb_o_Node *)tb_nn(v_n, 818))->q_name; long long tb_c891_1n = (long long)strlen(tb_c891_1); const char *tb_c891_2 = " = TB_BIT(tb_j->tb_l, tb_i);\n"; long long tb_c891_2n = sizeof(" = TB_BIT(tb_j->tb_l, tb_i);\n") - 1; char *tb_c891r = tb_mem(tb_c891_0n + tb_c891_1n + tb_c891_2n + 1), *tb_c891rw = tb_c891r; memcpy(tb_c891rw, tb_c891_0, tb_c891_0n); tb_c891rw += tb_c891_0n; memcpy(tb_c891rw, tb_c891_1, tb_c891_1n); tb_c891rw += tb_c891_1n; memcpy(tb_c891rw, tb_c891_2, tb_c891_2n); tb_c891rw += tb_c891_2n; *tb_c891rw = 0; (const char *)tb_c891r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 819))->q_kids, 1LL, sizeof(tb_o_Node *), 819)), 3LL);
    } else if (v_is_each) {
        f_emit__put(v_c, ({ const char *tb_c892_0 = "            "; long long tb_c892_0n = sizeof("            ") - 1; const char *tb_c892_1 = v_elem; long long tb_c892_1n = (long long)strlen(tb_c892_1); const char *tb_c892_2 = " const v_"; long long tb_c892_2n = sizeof(" const v_") - 1; const char *tb_c892_3 = ((tb_o_Node *)tb_nn(v_n, 821))->q_name; long long tb_c892_3n = (long long)strlen(tb_c892_3); const char *tb_c892_4 = " = (("; long long tb_c892_4n = sizeof(" = ((") - 1; const char *tb_c892_5 = v_elem; long long tb_c892_5n = (long long)strlen(tb_c892_5); const char *tb_c892_6 = " *)tb_j->tb_l->data)[tb_i];\n"; long long tb_c892_6n = sizeof(" *)tb_j->tb_l->data)[tb_i];\n") - 1; char *tb_c892r = tb_mem(tb_c892_0n + tb_c892_1n + tb_c892_2n + tb_c892_3n + tb_c892_4n + tb_c892_5n + tb_c892_6n + 1), *tb_c892rw = tb_c892r; memcpy(tb_c892rw, tb_c892_0, tb_c892_0n); tb_c892rw += tb_c892_0n; memcpy(tb_c892rw, tb_c892_1, tb_c892_1n); tb_c892rw += tb_c892_1n; memcpy(tb_c892rw, tb_c892_2, tb_c892_2n); tb_c892rw += tb_c892_2n; memcpy(tb_c892rw, tb_c892_3, tb_c892_3n); tb_c892rw += tb_c892_3n; memcpy(tb_c892rw, tb_c892_4, tb_c892_4n); tb_c892rw += tb_c892_4n; memcpy(tb_c892rw, tb_c892_5, tb_c892_5n); tb_c892rw += tb_c892_5n; memcpy(tb_c892rw, tb_c892_6, tb_c892_6n); tb_c892rw += tb_c892_6n; *tb_c892rw = 0; (const char *)tb_c892r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 822))->q_kids, 1LL, sizeof(tb_o_Node *), 822)), 3LL);
    } else {
        f_emit__put(v_c, ({ const char *tb_c893_0 = "            long long const v_"; long long tb_c893_0n = sizeof("            long long const v_") - 1; const char *tb_c893_1 = ((tb_o_Node *)tb_nn(v_n, 824))->q_name; long long tb_c893_1n = (long long)strlen(tb_c893_1); const char *tb_c893_2 = " = tb_j->tb_lo + tb_i;\n"; long long tb_c893_2n = sizeof(" = tb_j->tb_lo + tb_i;\n") - 1; char *tb_c893r = tb_mem(tb_c893_0n + tb_c893_1n + tb_c893_2n + 1), *tb_c893rw = tb_c893r; memcpy(tb_c893rw, tb_c893_0, tb_c893_0n); tb_c893rw += tb_c893_0n; memcpy(tb_c893rw, tb_c893_1, tb_c893_1n); tb_c893rw += tb_c893_1n; memcpy(tb_c893rw, tb_c893_2, tb_c893_2n); tb_c893rw += tb_c893_2n; *tb_c893rw = 0; (const char *)tb_c893r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 825))->q_kids, 2LL, sizeof(tb_o_Node *), 825)), 3LL);
    }
    f_emit__put(v_c, "        }\n    }\n");
    { tb_list * tb_v894 = v_saved_builders; tb_list * *tb_p894 = &(((tb_o_Comp *)tb_nn(v_c, 827))->q_builders); *tb_p894 = tb_v894; }
    if (v_has_shared) {
        f_emit__put(v_c, "    pthread_mutex_lock(&tb_par_lock);\n");
        {
            tb_list * tb_l895 = ((tb_o_Node *)tb_nn(v_n, 831))->q_caps;
            for (long long tb_i895 = 0; tb_i895 < tb_l895->len; tb_i895++) {
                tb_o_Var * const v_v = ((tb_o_Var * *)tb_l895->data)[tb_i895];
                if ((((tb_o_Var *)tb_nn(v_v, 832))->q_is_shared && ({ const char *tb_s896 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 832))->q_type); ((unsigned char)tb_s896[0] == 116 && (unsigned char)tb_s896[1] == 101 && (unsigned char)tb_s896[2] == 120 && (unsigned char)tb_s896[3] == 116 && tb_s896[4] == 0); }))) {
                    f_emit__put(v_c, ({ const char *tb_c897_0 = "    tb_app_take(&tb_j->m_"; long long tb_c897_0n = sizeof("    tb_app_take(&tb_j->m_") - 1; const char *tb_c897_1 = ((tb_o_Var *)tb_nn(v_v, 833))->q_name; long long tb_c897_1n = (long long)strlen(tb_c897_1); const char *tb_c897_2 = ", &tb_j->m_rl_"; long long tb_c897_2n = sizeof(", &tb_j->m_rl_") - 1; const char *tb_c897_3 = ((tb_o_Var *)tb_nn(v_v, 833))->q_name; long long tb_c897_3n = (long long)strlen(tb_c897_3); const char *tb_c897_4 = ", &tb_j->m_rc_"; long long tb_c897_4n = sizeof(", &tb_j->m_rc_") - 1; const char *tb_c897_5 = ((tb_o_Var *)tb_nn(v_v, 833))->q_name; long long tb_c897_5n = (long long)strlen(tb_c897_5); const char *tb_c897_6 = ", v_"; long long tb_c897_6n = sizeof(", v_") - 1; const char *tb_c897_7 = ((tb_o_Var *)tb_nn(v_v, 833))->q_name; long long tb_c897_7n = (long long)strlen(tb_c897_7); const char *tb_c897_8 = ", tb_rl_"; long long tb_c897_8n = sizeof(", tb_rl_") - 1; const char *tb_c897_9 = ((tb_o_Var *)tb_nn(v_v, 833))->q_name; long long tb_c897_9n = (long long)strlen(tb_c897_9); const char *tb_c897_10 = ", tb_rc_"; long long tb_c897_10n = sizeof(", tb_rc_") - 1; const char *tb_c897_11 = ((tb_o_Var *)tb_nn(v_v, 833))->q_name; long long tb_c897_11n = (long long)strlen(tb_c897_11); const char *tb_c897_12 = ");\n"; long long tb_c897_12n = sizeof(");\n") - 1; char *tb_c897r = tb_mem(tb_c897_0n + tb_c897_1n + tb_c897_2n + tb_c897_3n + tb_c897_4n + tb_c897_5n + tb_c897_6n + tb_c897_7n + tb_c897_8n + tb_c897_9n + tb_c897_10n + tb_c897_11n + tb_c897_12n + 1), *tb_c897rw = tb_c897r; memcpy(tb_c897rw, tb_c897_0, tb_c897_0n); tb_c897rw += tb_c897_0n; memcpy(tb_c897rw, tb_c897_1, tb_c897_1n); tb_c897rw += tb_c897_1n; memcpy(tb_c897rw, tb_c897_2, tb_c897_2n); tb_c897rw += tb_c897_2n; memcpy(tb_c897rw, tb_c897_3, tb_c897_3n); tb_c897rw += tb_c897_3n; memcpy(tb_c897rw, tb_c897_4, tb_c897_4n); tb_c897rw += tb_c897_4n; memcpy(tb_c897rw, tb_c897_5, tb_c897_5n); tb_c897rw += tb_c897_5n; memcpy(tb_c897rw, tb_c897_6, tb_c897_6n); tb_c897rw += tb_c897_6n; memcpy(tb_c897rw, tb_c897_7, tb_c897_7n); tb_c897rw += tb_c897_7n; memcpy(tb_c897rw, tb_c897_8, tb_c897_8n); tb_c897rw += tb_c897_8n; memcpy(tb_c897rw, tb_c897_9, tb_c897_9n); tb_c897rw += tb_c897_9n; memcpy(tb_c897rw, tb_c897_10, tb_c897_10n); tb_c897rw += tb_c897_10n; memcpy(tb_c897rw, tb_c897_11, tb_c897_11n); tb_c897rw += tb_c897_11n; memcpy(tb_c897rw, tb_c897_12, tb_c897_12n); tb_c897rw += tb_c897_12n; *tb_c897rw = 0; (const char *)tb_c897r; }));
                } else if (((tb_o_Var *)tb_nn(v_v, 834))->q_is_shared) {
                    f_emit__put(v_c, ({ const char *tb_c898_0 = "    *tb_j->p_"; long long tb_c898_0n = sizeof("    *tb_j->p_") - 1; const char *tb_c898_1 = ((tb_o_Var *)tb_nn(v_v, 835))->q_name; long long tb_c898_1n = (long long)strlen(tb_c898_1); const char *tb_c898_2 = " += v_"; long long tb_c898_2n = sizeof(" += v_") - 1; const char *tb_c898_3 = ((tb_o_Var *)tb_nn(v_v, 835))->q_name; long long tb_c898_3n = (long long)strlen(tb_c898_3); const char *tb_c898_4 = ";\n"; long long tb_c898_4n = sizeof(";\n") - 1; char *tb_c898r = tb_mem(tb_c898_0n + tb_c898_1n + tb_c898_2n + tb_c898_3n + tb_c898_4n + 1), *tb_c898rw = tb_c898r; memcpy(tb_c898rw, tb_c898_0, tb_c898_0n); tb_c898rw += tb_c898_0n; memcpy(tb_c898rw, tb_c898_1, tb_c898_1n); tb_c898rw += tb_c898_1n; memcpy(tb_c898rw, tb_c898_2, tb_c898_2n); tb_c898rw += tb_c898_2n; memcpy(tb_c898rw, tb_c898_3, tb_c898_3n); tb_c898rw += tb_c898_3n; memcpy(tb_c898rw, tb_c898_4, tb_c898_4n); tb_c898rw += tb_c898_4n; *tb_c898rw = 0; (const char *)tb_c898r; }));
                }
            }
        }
        f_emit__put(v_c, "    pthread_mutex_unlock(&tb_par_lock);\n");
    }
    f_emit__put(v_c, "    return 0;\n}\n");
    { const char * tb_v899 = tb_join(v_w, ""); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 838))->q_workers) = tb_v899; }
    { tb_list * tb_v900 = v_saved; tb_list * *tb_p900 = &(((tb_o_Comp *)tb_nn(v_c, 839))->q_out); *tb_p900 = tb_v900; }
    const char * v_j = ({ const char *tb_c901_0 = "tb_j"; long long tb_c901_0n = sizeof("tb_j") - 1; const char *tb_c901_1 = v_id; long long tb_c901_1n = (long long)strlen(tb_c901_1); char *tb_c901r = tb_mem(tb_c901_0n + tb_c901_1n + 1), *tb_c901rw = tb_c901r; memcpy(tb_c901rw, tb_c901_0, tb_c901_0n); tb_c901rw += tb_c901_0n; memcpy(tb_c901rw, tb_c901_1, tb_c901_1n); tb_c901rw += tb_c901_1n; *tb_c901rw = 0; (const char *)tb_c901r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c902_0 = "tb_job"; long long tb_c902_0n = sizeof("tb_job") - 1; const char *tb_c902_1 = v_id; long long tb_c902_1n = (long long)strlen(tb_c902_1); const char *tb_c902_2 = " "; long long tb_c902_2n = sizeof(" ") - 1; const char *tb_c902_3 = v_j; long long tb_c902_3n = (long long)strlen(tb_c902_3); const char *tb_c902_4 = " = {0};\n"; long long tb_c902_4n = sizeof(" = {0};\n") - 1; char *tb_c902r = tb_mem(tb_c902_0n + tb_c902_1n + tb_c902_2n + tb_c902_3n + tb_c902_4n + 1), *tb_c902rw = tb_c902r; memcpy(tb_c902rw, tb_c902_0, tb_c902_0n); tb_c902rw += tb_c902_0n; memcpy(tb_c902rw, tb_c902_1, tb_c902_1n); tb_c902rw += tb_c902_1n; memcpy(tb_c902rw, tb_c902_2, tb_c902_2n); tb_c902rw += tb_c902_2n; memcpy(tb_c902rw, tb_c902_3, tb_c902_3n); tb_c902rw += tb_c902_3n; memcpy(tb_c902rw, tb_c902_4, tb_c902_4n); tb_c902rw += tb_c902_4n; *tb_c902rw = 0; (const char *)tb_c902r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    if (v_is_each) {
        f_emit__put(v_c, ({ const char *tb_c903_0 = v_j; long long tb_c903_0n = (long long)strlen(tb_c903_0); const char *tb_c903_1 = ".tb_l = "; long long tb_c903_1n = sizeof(".tb_l = ") - 1; char *tb_c903r = tb_mem(tb_c903_0n + tb_c903_1n + 1), *tb_c903rw = tb_c903r; memcpy(tb_c903rw, tb_c903_0, tb_c903_0n); tb_c903rw += tb_c903_0n; memcpy(tb_c903rw, tb_c903_1, tb_c903_1n); tb_c903rw += tb_c903_1n; *tb_c903rw = 0; (const char *)tb_c903r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 849))->q_kids, 0LL, sizeof(tb_o_Node *), 849)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c904_0 = v_j; long long tb_c904_0n = (long long)strlen(tb_c904_0); const char *tb_c904_1 = ".tb_total = "; long long tb_c904_1n = sizeof(".tb_total = ") - 1; const char *tb_c904_2 = v_j; long long tb_c904_2n = (long long)strlen(tb_c904_2); const char *tb_c904_3 = ".tb_l->len;\n"; long long tb_c904_3n = sizeof(".tb_l->len;\n") - 1; char *tb_c904r = tb_mem(tb_c904_0n + tb_c904_1n + tb_c904_2n + tb_c904_3n + 1), *tb_c904rw = tb_c904r; memcpy(tb_c904rw, tb_c904_0, tb_c904_0n); tb_c904rw += tb_c904_0n; memcpy(tb_c904rw, tb_c904_1, tb_c904_1n); tb_c904rw += tb_c904_1n; memcpy(tb_c904rw, tb_c904_2, tb_c904_2n); tb_c904rw += tb_c904_2n; memcpy(tb_c904rw, tb_c904_3, tb_c904_3n); tb_c904rw += tb_c904_3n; *tb_c904rw = 0; (const char *)tb_c904r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c905_0 = "long long tb_end"; long long tb_c905_0n = sizeof("long long tb_end") - 1; const char *tb_c905_1 = v_id; long long tb_c905_1n = (long long)strlen(tb_c905_1); const char *tb_c905_2 = " = "; long long tb_c905_2n = sizeof(" = ") - 1; char *tb_c905r = tb_mem(tb_c905_0n + tb_c905_1n + tb_c905_2n + 1), *tb_c905rw = tb_c905r; memcpy(tb_c905rw, tb_c905_0, tb_c905_0n); tb_c905rw += tb_c905_0n; memcpy(tb_c905rw, tb_c905_1, tb_c905_1n); tb_c905rw += tb_c905_1n; memcpy(tb_c905rw, tb_c905_2, tb_c905_2n); tb_c905rw += tb_c905_2n; *tb_c905rw = 0; (const char *)tb_c905r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 855))->q_kids, 1LL, sizeof(tb_o_Node *), 855)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c906_0 = v_j; long long tb_c906_0n = (long long)strlen(tb_c906_0); const char *tb_c906_1 = ".tb_lo = "; long long tb_c906_1n = sizeof(".tb_lo = ") - 1; char *tb_c906r = tb_mem(tb_c906_0n + tb_c906_1n + 1), *tb_c906rw = tb_c906r; memcpy(tb_c906rw, tb_c906_0, tb_c906_0n); tb_c906rw += tb_c906_0n; memcpy(tb_c906rw, tb_c906_1, tb_c906_1n); tb_c906rw += tb_c906_1n; *tb_c906rw = 0; (const char *)tb_c906r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 859))->q_kids, 0LL, sizeof(tb_o_Node *), 859)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c907_0 = v_j; long long tb_c907_0n = (long long)strlen(tb_c907_0); const char *tb_c907_1 = ".tb_total = tb_end"; long long tb_c907_1n = sizeof(".tb_total = tb_end") - 1; const char *tb_c907_2 = v_id; long long tb_c907_2n = (long long)strlen(tb_c907_2); const char *tb_c907_3 = " < "; long long tb_c907_3n = sizeof(" < ") - 1; const char *tb_c907_4 = v_j; long long tb_c907_4n = (long long)strlen(tb_c907_4); const char *tb_c907_5 = ".tb_lo ? 0 : tb_end"; long long tb_c907_5n = sizeof(".tb_lo ? 0 : tb_end") - 1; const char *tb_c907_6 = v_id; long long tb_c907_6n = (long long)strlen(tb_c907_6); const char *tb_c907_7 = " - "; long long tb_c907_7n = sizeof(" - ") - 1; const char *tb_c907_8 = v_j; long long tb_c907_8n = (long long)strlen(tb_c907_8); const char *tb_c907_9 = ".tb_lo + 1;\n"; long long tb_c907_9n = sizeof(".tb_lo + 1;\n") - 1; char *tb_c907r = tb_mem(tb_c907_0n + tb_c907_1n + tb_c907_2n + tb_c907_3n + tb_c907_4n + tb_c907_5n + tb_c907_6n + tb_c907_7n + tb_c907_8n + tb_c907_9n + 1), *tb_c907rw = tb_c907r; memcpy(tb_c907rw, tb_c907_0, tb_c907_0n); tb_c907rw += tb_c907_0n; memcpy(tb_c907rw, tb_c907_1, tb_c907_1n); tb_c907rw += tb_c907_1n; memcpy(tb_c907rw, tb_c907_2, tb_c907_2n); tb_c907rw += tb_c907_2n; memcpy(tb_c907rw, tb_c907_3, tb_c907_3n); tb_c907rw += tb_c907_3n; memcpy(tb_c907rw, tb_c907_4, tb_c907_4n); tb_c907rw += tb_c907_4n; memcpy(tb_c907rw, tb_c907_5, tb_c907_5n); tb_c907rw += tb_c907_5n; memcpy(tb_c907rw, tb_c907_6, tb_c907_6n); tb_c907rw += tb_c907_6n; memcpy(tb_c907rw, tb_c907_7, tb_c907_7n); tb_c907rw += tb_c907_7n; memcpy(tb_c907rw, tb_c907_8, tb_c907_8n); tb_c907rw += tb_c907_8n; memcpy(tb_c907rw, tb_c907_9, tb_c907_9n); tb_c907rw += tb_c907_9n; *tb_c907rw = 0; (const char *)tb_c907r; }));
    }
    {
        tb_list * tb_l908 = ((tb_o_Node *)tb_nn(v_n, 863))->q_caps;
        for (long long tb_i908 = 0; tb_i908 < tb_l908->len; tb_i908++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l908->data)[tb_i908];
            f_emit__indent(v_c, (v_depth + 1LL));
            if ((((tb_o_Var *)tb_nn(v_v, 865))->q_is_shared && ({ const char *tb_s909 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 865))->q_type); ((unsigned char)tb_s909[0] == 116 && (unsigned char)tb_s909[1] == 101 && (unsigned char)tb_s909[2] == 120 && (unsigned char)tb_s909[3] == 116 && tb_s909[4] == 0); }))) {
                f_emit__put(v_c, ({ const char *tb_c910_0 = v_j; long long tb_c910_0n = (long long)strlen(tb_c910_0); const char *tb_c910_1 = ".m_"; long long tb_c910_1n = sizeof(".m_") - 1; const char *tb_c910_2 = ((tb_o_Var *)tb_nn(v_v, 866))->q_name; long long tb_c910_2n = (long long)strlen(tb_c910_2); const char *tb_c910_3 = " = v_"; long long tb_c910_3n = sizeof(" = v_") - 1; const char *tb_c910_4 = ((tb_o_Var *)tb_nn(v_v, 866))->q_name; long long tb_c910_4n = (long long)strlen(tb_c910_4); const char *tb_c910_5 = "; "; long long tb_c910_5n = sizeof("; ") - 1; const char *tb_c910_6 = v_j; long long tb_c910_6n = (long long)strlen(tb_c910_6); const char *tb_c910_7 = ".m_rl_"; long long tb_c910_7n = sizeof(".m_rl_") - 1; const char *tb_c910_8 = ((tb_o_Var *)tb_nn(v_v, 866))->q_name; long long tb_c910_8n = (long long)strlen(tb_c910_8); const char *tb_c910_9 = " = -1;\n"; long long tb_c910_9n = sizeof(" = -1;\n") - 1; char *tb_c910r = tb_mem(tb_c910_0n + tb_c910_1n + tb_c910_2n + tb_c910_3n + tb_c910_4n + tb_c910_5n + tb_c910_6n + tb_c910_7n + tb_c910_8n + tb_c910_9n + 1), *tb_c910rw = tb_c910r; memcpy(tb_c910rw, tb_c910_0, tb_c910_0n); tb_c910rw += tb_c910_0n; memcpy(tb_c910rw, tb_c910_1, tb_c910_1n); tb_c910rw += tb_c910_1n; memcpy(tb_c910rw, tb_c910_2, tb_c910_2n); tb_c910rw += tb_c910_2n; memcpy(tb_c910rw, tb_c910_3, tb_c910_3n); tb_c910rw += tb_c910_3n; memcpy(tb_c910rw, tb_c910_4, tb_c910_4n); tb_c910rw += tb_c910_4n; memcpy(tb_c910rw, tb_c910_5, tb_c910_5n); tb_c910rw += tb_c910_5n; memcpy(tb_c910rw, tb_c910_6, tb_c910_6n); tb_c910rw += tb_c910_6n; memcpy(tb_c910rw, tb_c910_7, tb_c910_7n); tb_c910rw += tb_c910_7n; memcpy(tb_c910rw, tb_c910_8, tb_c910_8n); tb_c910rw += tb_c910_8n; memcpy(tb_c910rw, tb_c910_9, tb_c910_9n); tb_c910rw += tb_c910_9n; *tb_c910rw = 0; (const char *)tb_c910r; }));
            } else if (((tb_o_Var *)tb_nn(v_v, 867))->q_is_shared) {
                f_emit__put(v_c, ({ const char *tb_c911_0 = v_j; long long tb_c911_0n = (long long)strlen(tb_c911_0); const char *tb_c911_1 = ".p_"; long long tb_c911_1n = sizeof(".p_") - 1; const char *tb_c911_2 = ((tb_o_Var *)tb_nn(v_v, 868))->q_name; long long tb_c911_2n = (long long)strlen(tb_c911_2); const char *tb_c911_3 = " = &v_"; long long tb_c911_3n = sizeof(" = &v_") - 1; const char *tb_c911_4 = ((tb_o_Var *)tb_nn(v_v, 868))->q_name; long long tb_c911_4n = (long long)strlen(tb_c911_4); const char *tb_c911_5 = ";\n"; long long tb_c911_5n = sizeof(";\n") - 1; char *tb_c911r = tb_mem(tb_c911_0n + tb_c911_1n + tb_c911_2n + tb_c911_3n + tb_c911_4n + tb_c911_5n + 1), *tb_c911rw = tb_c911r; memcpy(tb_c911rw, tb_c911_0, tb_c911_0n); tb_c911rw += tb_c911_0n; memcpy(tb_c911rw, tb_c911_1, tb_c911_1n); tb_c911rw += tb_c911_1n; memcpy(tb_c911rw, tb_c911_2, tb_c911_2n); tb_c911rw += tb_c911_2n; memcpy(tb_c911rw, tb_c911_3, tb_c911_3n); tb_c911rw += tb_c911_3n; memcpy(tb_c911rw, tb_c911_4, tb_c911_4n); tb_c911rw += tb_c911_4n; memcpy(tb_c911rw, tb_c911_5, tb_c911_5n); tb_c911rw += tb_c911_5n; *tb_c911rw = 0; (const char *)tb_c911r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c912_0 = v_j; long long tb_c912_0n = (long long)strlen(tb_c912_0); const char *tb_c912_1 = ".v_"; long long tb_c912_1n = sizeof(".v_") - 1; const char *tb_c912_2 = ((tb_o_Var *)tb_nn(v_v, 870))->q_name; long long tb_c912_2n = (long long)strlen(tb_c912_2); const char *tb_c912_3 = " = v_"; long long tb_c912_3n = sizeof(" = v_") - 1; const char *tb_c912_4 = ((tb_o_Var *)tb_nn(v_v, 870))->q_name; long long tb_c912_4n = (long long)strlen(tb_c912_4); const char *tb_c912_5 = ";\n"; long long tb_c912_5n = sizeof(";\n") - 1; char *tb_c912r = tb_mem(tb_c912_0n + tb_c912_1n + tb_c912_2n + tb_c912_3n + tb_c912_4n + tb_c912_5n + 1), *tb_c912rw = tb_c912r; memcpy(tb_c912rw, tb_c912_0, tb_c912_0n); tb_c912rw += tb_c912_0n; memcpy(tb_c912rw, tb_c912_1, tb_c912_1n); tb_c912rw += tb_c912_1n; memcpy(tb_c912rw, tb_c912_2, tb_c912_2n); tb_c912rw += tb_c912_2n; memcpy(tb_c912rw, tb_c912_3, tb_c912_3n); tb_c912rw += tb_c912_3n; memcpy(tb_c912rw, tb_c912_4, tb_c912_4n); tb_c912rw += tb_c912_4n; memcpy(tb_c912rw, tb_c912_5, tb_c912_5n); tb_c912rw += tb_c912_5n; *tb_c912rw = 0; (const char *)tb_c912r; }));
            }
        }
    }
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c913_0 = v_j; long long tb_c913_0n = (long long)strlen(tb_c913_0); const char *tb_c913_1 = ".tb_step = tb_par_step("; long long tb_c913_1n = sizeof(".tb_step = tb_par_step(") - 1; const char *tb_c913_2 = v_j; long long tb_c913_2n = (long long)strlen(tb_c913_2); const char *tb_c913_3 = ".tb_total);\n"; long long tb_c913_3n = sizeof(".tb_total);\n") - 1; char *tb_c913r = tb_mem(tb_c913_0n + tb_c913_1n + tb_c913_2n + tb_c913_3n + 1), *tb_c913rw = tb_c913r; memcpy(tb_c913rw, tb_c913_0, tb_c913_0n); tb_c913rw += tb_c913_0n; memcpy(tb_c913rw, tb_c913_1, tb_c913_1n); tb_c913rw += tb_c913_1n; memcpy(tb_c913rw, tb_c913_2, tb_c913_2n); tb_c913rw += tb_c913_2n; memcpy(tb_c913rw, tb_c913_3, tb_c913_3n); tb_c913rw += tb_c913_3n; *tb_c913rw = 0; (const char *)tb_c913r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c914_0 = "tb_par_run(tb_work"; long long tb_c914_0n = sizeof("tb_par_run(tb_work") - 1; const char *tb_c914_1 = v_id; long long tb_c914_1n = (long long)strlen(tb_c914_1); const char *tb_c914_2 = ", &"; long long tb_c914_2n = sizeof(", &") - 1; const char *tb_c914_3 = v_j; long long tb_c914_3n = (long long)strlen(tb_c914_3); const char *tb_c914_4 = ", "; long long tb_c914_4n = sizeof(", ") - 1; const char *tb_c914_5 = v_j; long long tb_c914_5n = (long long)strlen(tb_c914_5); const char *tb_c914_6 = ".tb_total);\n"; long long tb_c914_6n = sizeof(".tb_total);\n") - 1; char *tb_c914r = tb_mem(tb_c914_0n + tb_c914_1n + tb_c914_2n + tb_c914_3n + tb_c914_4n + tb_c914_5n + tb_c914_6n + 1), *tb_c914rw = tb_c914r; memcpy(tb_c914rw, tb_c914_0, tb_c914_0n); tb_c914rw += tb_c914_0n; memcpy(tb_c914rw, tb_c914_1, tb_c914_1n); tb_c914rw += tb_c914_1n; memcpy(tb_c914rw, tb_c914_2, tb_c914_2n); tb_c914rw += tb_c914_2n; memcpy(tb_c914rw, tb_c914_3, tb_c914_3n); tb_c914rw += tb_c914_3n; memcpy(tb_c914rw, tb_c914_4, tb_c914_4n); tb_c914rw += tb_c914_4n; memcpy(tb_c914rw, tb_c914_5, tb_c914_5n); tb_c914rw += tb_c914_5n; memcpy(tb_c914rw, tb_c914_6, tb_c914_6n); tb_c914rw += tb_c914_6n; *tb_c914rw = 0; (const char *)tb_c914r; }));
    {
        tb_list * tb_l915 = ((tb_o_Node *)tb_nn(v_n, 875))->q_caps;
        for (long long tb_i915 = 0; tb_i915 < tb_l915->len; tb_i915++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l915->data)[tb_i915];
            if ((((tb_o_Var *)tb_nn(v_v, 876))->q_is_shared && ({ const char *tb_s916 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 876))->q_type); ((unsigned char)tb_s916[0] == 116 && (unsigned char)tb_s916[1] == 101 && (unsigned char)tb_s916[2] == 120 && (unsigned char)tb_s916[3] == 116 && tb_s916[4] == 0); }))) {
                f_emit__indent(v_c, (v_depth + 1LL));
                f_emit__put(v_c, ({ const char *tb_c917_0 = "v_"; long long tb_c917_0n = sizeof("v_") - 1; const char *tb_c917_1 = ((tb_o_Var *)tb_nn(v_v, 878))->q_name; long long tb_c917_1n = (long long)strlen(tb_c917_1); const char *tb_c917_2 = " = "; long long tb_c917_2n = sizeof(" = ") - 1; const char *tb_c917_3 = v_j; long long tb_c917_3n = (long long)strlen(tb_c917_3); const char *tb_c917_4 = ".m_"; long long tb_c917_4n = sizeof(".m_") - 1; const char *tb_c917_5 = ((tb_o_Var *)tb_nn(v_v, 878))->q_name; long long tb_c917_5n = (long long)strlen(tb_c917_5); const char *tb_c917_6 = ";\n"; long long tb_c917_6n = sizeof(";\n") - 1; char *tb_c917r = tb_mem(tb_c917_0n + tb_c917_1n + tb_c917_2n + tb_c917_3n + tb_c917_4n + tb_c917_5n + tb_c917_6n + 1), *tb_c917rw = tb_c917r; memcpy(tb_c917rw, tb_c917_0, tb_c917_0n); tb_c917rw += tb_c917_0n; memcpy(tb_c917rw, tb_c917_1, tb_c917_1n); tb_c917rw += tb_c917_1n; memcpy(tb_c917rw, tb_c917_2, tb_c917_2n); tb_c917rw += tb_c917_2n; memcpy(tb_c917rw, tb_c917_3, tb_c917_3n); tb_c917rw += tb_c917_3n; memcpy(tb_c917rw, tb_c917_4, tb_c917_4n); tb_c917rw += tb_c917_4n; memcpy(tb_c917rw, tb_c917_5, tb_c917_5n); tb_c917rw += tb_c917_5n; memcpy(tb_c917rw, tb_c917_6, tb_c917_6n); tb_c917rw += tb_c917_6n; *tb_c917rw = 0; (const char *)tb_c917r; }));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static long long f_emit__vec_index(tb_o_Comp * v_c, const char * v_name) {
    {
        long long tb_end918 = ((((tb_o_Comp *)tb_nn(v_c, 883))->q_vec_names)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end918; v_i++) {
            if ((strcmp((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 884))->q_vec_names, v_i, sizeof(const char *), 884)), v_name) == 0)) {
                return v_i;
            }
        }
    }
    return (-1LL);
    tb_crash(882, "функция vec_index дошла до конца без return");
    return 0LL;
}

static void f_emit__emit_vec(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v919 = 1LL; long long *tb_p919 = &(((tb_o_Comp *)tb_nn(v_c, 891))->q_label); *tb_p919 += tb_v919; }
    const char * v_id = ({ const char *tb_c920_0 = ""; long long tb_c920_0n = sizeof("") - 1; char tb_c920_1d[24]; long long tb_c920_1n; const char *tb_c920_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 892))->q_label, tb_c920_1d, &tb_c920_1n); char *tb_c920r = tb_mem(tb_c920_0n + tb_c920_1n + 1), *tb_c920rw = tb_c920r; memcpy(tb_c920rw, tb_c920_0, tb_c920_0n); tb_c920rw += tb_c920_0n; memcpy(tb_c920rw, tb_c920_1, tb_c920_1n); tb_c920rw += tb_c920_1n; *tb_c920rw = 0; (const char *)tb_c920r; });
    const char * v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 893))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 893))->q_kids, 0LL, sizeof(tb_o_Node *), 893)), 893))->q_ty, sizeof(tb_o_Type *), 893)), 893))->q_elem);
    long long v_d = (v_depth + 1LL);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c921_0 = "tb_list *tb_l"; long long tb_c921_0n = sizeof("tb_list *tb_l") - 1; const char *tb_c921_1 = v_id; long long tb_c921_1n = (long long)strlen(tb_c921_1); const char *tb_c921_2 = " = "; long long tb_c921_2n = sizeof(" = ") - 1; char *tb_c921r = tb_mem(tb_c921_0n + tb_c921_1n + tb_c921_2n + 1), *tb_c921rw = tb_c921r; memcpy(tb_c921rw, tb_c921_0, tb_c921_0n); tb_c921rw += tb_c921_0n; memcpy(tb_c921rw, tb_c921_1, tb_c921_1n); tb_c921rw += tb_c921_1n; memcpy(tb_c921rw, tb_c921_2, tb_c921_2n); tb_c921rw += tb_c921_2n; *tb_c921rw = 0; (const char *)tb_c921r; }));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 899))->q_kids, 0LL, sizeof(tb_o_Node *), 899)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c922_0 = v_elem; long long tb_c922_0n = (long long)strlen(tb_c922_0); const char *tb_c922_1 = " *tb_d"; long long tb_c922_1n = sizeof(" *tb_d") - 1; const char *tb_c922_2 = v_id; long long tb_c922_2n = (long long)strlen(tb_c922_2); const char *tb_c922_3 = " = ("; long long tb_c922_3n = sizeof(" = (") - 1; const char *tb_c922_4 = v_elem; long long tb_c922_4n = (long long)strlen(tb_c922_4); const char *tb_c922_5 = " *)tb_l"; long long tb_c922_5n = sizeof(" *)tb_l") - 1; const char *tb_c922_6 = v_id; long long tb_c922_6n = (long long)strlen(tb_c922_6); const char *tb_c922_7 = "->data;\n"; long long tb_c922_7n = sizeof("->data;\n") - 1; char *tb_c922r = tb_mem(tb_c922_0n + tb_c922_1n + tb_c922_2n + tb_c922_3n + tb_c922_4n + tb_c922_5n + tb_c922_6n + tb_c922_7n + 1), *tb_c922rw = tb_c922r; memcpy(tb_c922rw, tb_c922_0, tb_c922_0n); tb_c922rw += tb_c922_0n; memcpy(tb_c922rw, tb_c922_1, tb_c922_1n); tb_c922rw += tb_c922_1n; memcpy(tb_c922rw, tb_c922_2, tb_c922_2n); tb_c922rw += tb_c922_2n; memcpy(tb_c922rw, tb_c922_3, tb_c922_3n); tb_c922rw += tb_c922_3n; memcpy(tb_c922rw, tb_c922_4, tb_c922_4n); tb_c922rw += tb_c922_4n; memcpy(tb_c922rw, tb_c922_5, tb_c922_5n); tb_c922rw += tb_c922_5n; memcpy(tb_c922rw, tb_c922_6, tb_c922_6n); tb_c922rw += tb_c922_6n; memcpy(tb_c922rw, tb_c922_7, tb_c922_7n); tb_c922rw += tb_c922_7n; *tb_c922rw = 0; (const char *)tb_c922r; }));
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c923_0 = "long long tb_n"; long long tb_c923_0n = sizeof("long long tb_n") - 1; const char *tb_c923_1 = v_id; long long tb_c923_1n = (long long)strlen(tb_c923_1); const char *tb_c923_2 = " = tb_l"; long long tb_c923_2n = sizeof(" = tb_l") - 1; const char *tb_c923_3 = v_id; long long tb_c923_3n = (long long)strlen(tb_c923_3); const char *tb_c923_4 = "->len, tb_i"; long long tb_c923_4n = sizeof("->len, tb_i") - 1; const char *tb_c923_5 = v_id; long long tb_c923_5n = (long long)strlen(tb_c923_5); const char *tb_c923_6 = " = 0;\n"; long long tb_c923_6n = sizeof(" = 0;\n") - 1; char *tb_c923r = tb_mem(tb_c923_0n + tb_c923_1n + tb_c923_2n + tb_c923_3n + tb_c923_4n + tb_c923_5n + tb_c923_6n + 1), *tb_c923rw = tb_c923r; memcpy(tb_c923rw, tb_c923_0, tb_c923_0n); tb_c923rw += tb_c923_0n; memcpy(tb_c923rw, tb_c923_1, tb_c923_1n); tb_c923rw += tb_c923_1n; memcpy(tb_c923rw, tb_c923_2, tb_c923_2n); tb_c923rw += tb_c923_2n; memcpy(tb_c923rw, tb_c923_3, tb_c923_3n); tb_c923rw += tb_c923_3n; memcpy(tb_c923rw, tb_c923_4, tb_c923_4n); tb_c923rw += tb_c923_4n; memcpy(tb_c923rw, tb_c923_5, tb_c923_5n); tb_c923rw += tb_c923_5n; memcpy(tb_c923rw, tb_c923_6, tb_c923_6n); tb_c923rw += tb_c923_6n; *tb_c923rw = 0; (const char *)tb_c923r; }));
    {
        tb_list * tb_l924 = ((tb_o_Node *)tb_nn(v_n, 907))->q_caps;
        for (long long tb_i924 = 0; tb_i924 < tb_l924->len; tb_i924++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l924->data)[tb_i924];
            if (((tb_o_Var *)tb_nn(v_v, 908))->q_is_shared) {
                f_emit__indent(v_c, v_d);
                f_emit__put(v_c, ({ const char *tb_c925_0 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 910))->q_type); long long tb_c925_0n = (long long)strlen(tb_c925_0); const char *tb_c925_1 = " "; long long tb_c925_1n = sizeof(" ") - 1; char *tb_c925r = tb_mem(tb_c925_0n + tb_c925_1n + 1), *tb_c925rw = tb_c925r; memcpy(tb_c925rw, tb_c925_0, tb_c925_0n); tb_c925rw += tb_c925_0n; memcpy(tb_c925rw, tb_c925_1, tb_c925_1n); tb_c925rw += tb_c925_1n; *tb_c925rw = 0; (const char *)tb_c925r; }));
                {
                    long long tb_end926 = 7LL;
                    for (long long v_k = 0LL; v_k <= tb_end926; v_k++) {
                        if ((v_k > 0LL)) {
                            f_emit__put(v_c, ", ");
                        }
                        f_emit__put(v_c, ({ const char *tb_c927_0 = "tb_a"; long long tb_c927_0n = sizeof("tb_a") - 1; const char *tb_c927_1 = v_id; long long tb_c927_1n = (long long)strlen(tb_c927_1); const char *tb_c927_2 = "_"; long long tb_c927_2n = sizeof("_") - 1; const char *tb_c927_3 = ((tb_o_Var *)tb_nn(v_v, 914))->q_name; long long tb_c927_3n = (long long)strlen(tb_c927_3); const char *tb_c927_4 = "_"; long long tb_c927_4n = sizeof("_") - 1; char tb_c927_5d[24]; long long tb_c927_5n; const char *tb_c927_5 = tb_digits(v_k, tb_c927_5d, &tb_c927_5n); const char *tb_c927_6 = " = 0"; long long tb_c927_6n = sizeof(" = 0") - 1; char *tb_c927r = tb_mem(tb_c927_0n + tb_c927_1n + tb_c927_2n + tb_c927_3n + tb_c927_4n + tb_c927_5n + tb_c927_6n + 1), *tb_c927rw = tb_c927r; memcpy(tb_c927rw, tb_c927_0, tb_c927_0n); tb_c927rw += tb_c927_0n; memcpy(tb_c927rw, tb_c927_1, tb_c927_1n); tb_c927rw += tb_c927_1n; memcpy(tb_c927rw, tb_c927_2, tb_c927_2n); tb_c927rw += tb_c927_2n; memcpy(tb_c927rw, tb_c927_3, tb_c927_3n); tb_c927rw += tb_c927_3n; memcpy(tb_c927rw, tb_c927_4, tb_c927_4n); tb_c927rw += tb_c927_4n; memcpy(tb_c927rw, tb_c927_5, tb_c927_5n); tb_c927rw += tb_c927_5n; memcpy(tb_c927rw, tb_c927_6, tb_c927_6n); tb_c927rw += tb_c927_6n; *tb_c927rw = 0; (const char *)tb_c927r; }));
                    }
                }
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c928_0 = "for (; tb_i"; long long tb_c928_0n = sizeof("for (; tb_i") - 1; const char *tb_c928_1 = v_id; long long tb_c928_1n = (long long)strlen(tb_c928_1); const char *tb_c928_2 = " + 8 <= tb_n"; long long tb_c928_2n = sizeof(" + 8 <= tb_n") - 1; const char *tb_c928_3 = v_id; long long tb_c928_3n = (long long)strlen(tb_c928_3); const char *tb_c928_4 = "; tb_i"; long long tb_c928_4n = sizeof("; tb_i") - 1; const char *tb_c928_5 = v_id; long long tb_c928_5n = (long long)strlen(tb_c928_5); const char *tb_c928_6 = " += 8) {\n"; long long tb_c928_6n = sizeof(" += 8) {\n") - 1; char *tb_c928r = tb_mem(tb_c928_0n + tb_c928_1n + tb_c928_2n + tb_c928_3n + tb_c928_4n + tb_c928_5n + tb_c928_6n + 1), *tb_c928rw = tb_c928r; memcpy(tb_c928rw, tb_c928_0, tb_c928_0n); tb_c928rw += tb_c928_0n; memcpy(tb_c928rw, tb_c928_1, tb_c928_1n); tb_c928rw += tb_c928_1n; memcpy(tb_c928rw, tb_c928_2, tb_c928_2n); tb_c928rw += tb_c928_2n; memcpy(tb_c928rw, tb_c928_3, tb_c928_3n); tb_c928rw += tb_c928_3n; memcpy(tb_c928rw, tb_c928_4, tb_c928_4n); tb_c928rw += tb_c928_4n; memcpy(tb_c928rw, tb_c928_5, tb_c928_5n); tb_c928rw += tb_c928_5n; memcpy(tb_c928rw, tb_c928_6, tb_c928_6n); tb_c928rw += tb_c928_6n; *tb_c928rw = 0; (const char *)tb_c928r; }));
    {
        long long tb_end929 = 7LL;
        for (long long v_k = 0LL; v_k <= tb_end929; v_k++) {
            f_emit__indent(v_c, (v_d + 1LL));
            f_emit__put(v_c, "do {\n");
            f_emit__indent(v_c, (v_d + 2LL));
            f_emit__put(v_c, ({ const char *tb_c930_0 = v_elem; long long tb_c930_0n = (long long)strlen(tb_c930_0); const char *tb_c930_1 = " const v_"; long long tb_c930_1n = sizeof(" const v_") - 1; const char *tb_c930_2 = ((tb_o_Node *)tb_nn(v_n, 924))->q_name; long long tb_c930_2n = (long long)strlen(tb_c930_2); const char *tb_c930_3 = " = tb_d"; long long tb_c930_3n = sizeof(" = tb_d") - 1; const char *tb_c930_4 = v_id; long long tb_c930_4n = (long long)strlen(tb_c930_4); const char *tb_c930_5 = "[tb_i"; long long tb_c930_5n = sizeof("[tb_i") - 1; const char *tb_c930_6 = v_id; long long tb_c930_6n = (long long)strlen(tb_c930_6); const char *tb_c930_7 = " + "; long long tb_c930_7n = sizeof(" + ") - 1; char tb_c930_8d[24]; long long tb_c930_8n; const char *tb_c930_8 = tb_digits(v_k, tb_c930_8d, &tb_c930_8n); const char *tb_c930_9 = "];\n"; long long tb_c930_9n = sizeof("];\n") - 1; char *tb_c930r = tb_mem(tb_c930_0n + tb_c930_1n + tb_c930_2n + tb_c930_3n + tb_c930_4n + tb_c930_5n + tb_c930_6n + tb_c930_7n + tb_c930_8n + tb_c930_9n + 1), *tb_c930rw = tb_c930r; memcpy(tb_c930rw, tb_c930_0, tb_c930_0n); tb_c930rw += tb_c930_0n; memcpy(tb_c930rw, tb_c930_1, tb_c930_1n); tb_c930rw += tb_c930_1n; memcpy(tb_c930rw, tb_c930_2, tb_c930_2n); tb_c930rw += tb_c930_2n; memcpy(tb_c930rw, tb_c930_3, tb_c930_3n); tb_c930rw += tb_c930_3n; memcpy(tb_c930rw, tb_c930_4, tb_c930_4n); tb_c930rw += tb_c930_4n; memcpy(tb_c930rw, tb_c930_5, tb_c930_5n); tb_c930rw += tb_c930_5n; memcpy(tb_c930rw, tb_c930_6, tb_c930_6n); tb_c930rw += tb_c930_6n; memcpy(tb_c930rw, tb_c930_7, tb_c930_7n); tb_c930rw += tb_c930_7n; memcpy(tb_c930rw, tb_c930_8, tb_c930_8n); tb_c930rw += tb_c930_8n; memcpy(tb_c930rw, tb_c930_9, tb_c930_9n); tb_c930rw += tb_c930_9n; *tb_c930rw = 0; (const char *)tb_c930r; }));
            {
                tb_list * tb_l931 = ((tb_o_Node *)tb_nn(v_n, 925))->q_caps;
                for (long long tb_i931 = 0; tb_i931 < tb_l931->len; tb_i931++) {
                    tb_o_Var * const v_v = ((tb_o_Var * *)tb_l931->data)[tb_i931];
                    if (((tb_o_Var *)tb_nn(v_v, 926))->q_is_shared) {
                        { const char * tb_v932 = ((tb_o_Var *)tb_nn(v_v, 927))->q_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 927))->q_vec_names) = tb_v932; }
                        { const char * tb_v933 = ({ const char *tb_c934_0 = "tb_a"; long long tb_c934_0n = sizeof("tb_a") - 1; const char *tb_c934_1 = v_id; long long tb_c934_1n = (long long)strlen(tb_c934_1); const char *tb_c934_2 = "_"; long long tb_c934_2n = sizeof("_") - 1; const char *tb_c934_3 = ((tb_o_Var *)tb_nn(v_v, 928))->q_name; long long tb_c934_3n = (long long)strlen(tb_c934_3); const char *tb_c934_4 = "_"; long long tb_c934_4n = sizeof("_") - 1; char tb_c934_5d[24]; long long tb_c934_5n; const char *tb_c934_5 = tb_digits(v_k, tb_c934_5d, &tb_c934_5n); char *tb_c934r = tb_mem(tb_c934_0n + tb_c934_1n + tb_c934_2n + tb_c934_3n + tb_c934_4n + tb_c934_5n + 1), *tb_c934rw = tb_c934r; memcpy(tb_c934rw, tb_c934_0, tb_c934_0n); tb_c934rw += tb_c934_0n; memcpy(tb_c934rw, tb_c934_1, tb_c934_1n); tb_c934rw += tb_c934_1n; memcpy(tb_c934rw, tb_c934_2, tb_c934_2n); tb_c934rw += tb_c934_2n; memcpy(tb_c934rw, tb_c934_3, tb_c934_3n); tb_c934rw += tb_c934_3n; memcpy(tb_c934rw, tb_c934_4, tb_c934_4n); tb_c934rw += tb_c934_4n; memcpy(tb_c934rw, tb_c934_5, tb_c934_5n); tb_c934rw += tb_c934_5n; *tb_c934rw = 0; (const char *)tb_c934r; }); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 928))->q_vec_repl) = tb_v933; }
                    }
                }
            }
            f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 929))->q_kids, 1LL, sizeof(tb_o_Node *), 929)), (v_d + 2LL));
            tb_list * v_fresh_names = tb_list_new(sizeof(const char *));
            tb_list * v_fresh_repl = tb_list_new(sizeof(const char *));
            { tb_list * tb_v935 = v_fresh_names; tb_list * *tb_p935 = &(((tb_o_Comp *)tb_nn(v_c, 932))->q_vec_names); *tb_p935 = tb_v935; }
            { tb_list * tb_v936 = v_fresh_repl; tb_list * *tb_p936 = &(((tb_o_Comp *)tb_nn(v_c, 933))->q_vec_repl); *tb_p936 = tb_v936; }
            f_emit__indent(v_c, (v_d + 1LL));
            f_emit__put(v_c, "} while (0);\n");
        }
    }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c937_0 = "for (; tb_i"; long long tb_c937_0n = sizeof("for (; tb_i") - 1; const char *tb_c937_1 = v_id; long long tb_c937_1n = (long long)strlen(tb_c937_1); const char *tb_c937_2 = " < tb_n"; long long tb_c937_2n = sizeof(" < tb_n") - 1; const char *tb_c937_3 = v_id; long long tb_c937_3n = (long long)strlen(tb_c937_3); const char *tb_c937_4 = "; tb_i"; long long tb_c937_4n = sizeof("; tb_i") - 1; const char *tb_c937_5 = v_id; long long tb_c937_5n = (long long)strlen(tb_c937_5); const char *tb_c937_6 = "++) {\n"; long long tb_c937_6n = sizeof("++) {\n") - 1; char *tb_c937r = tb_mem(tb_c937_0n + tb_c937_1n + tb_c937_2n + tb_c937_3n + tb_c937_4n + tb_c937_5n + tb_c937_6n + 1), *tb_c937rw = tb_c937r; memcpy(tb_c937rw, tb_c937_0, tb_c937_0n); tb_c937rw += tb_c937_0n; memcpy(tb_c937rw, tb_c937_1, tb_c937_1n); tb_c937rw += tb_c937_1n; memcpy(tb_c937rw, tb_c937_2, tb_c937_2n); tb_c937rw += tb_c937_2n; memcpy(tb_c937rw, tb_c937_3, tb_c937_3n); tb_c937rw += tb_c937_3n; memcpy(tb_c937rw, tb_c937_4, tb_c937_4n); tb_c937rw += tb_c937_4n; memcpy(tb_c937rw, tb_c937_5, tb_c937_5n); tb_c937rw += tb_c937_5n; memcpy(tb_c937rw, tb_c937_6, tb_c937_6n); tb_c937rw += tb_c937_6n; *tb_c937rw = 0; (const char *)tb_c937r; }));
    f_emit__indent(v_c, (v_d + 1LL));
    f_emit__put(v_c, ({ const char *tb_c938_0 = v_elem; long long tb_c938_0n = (long long)strlen(tb_c938_0); const char *tb_c938_1 = " const v_"; long long tb_c938_1n = sizeof(" const v_") - 1; const char *tb_c938_2 = ((tb_o_Node *)tb_nn(v_n, 942))->q_name; long long tb_c938_2n = (long long)strlen(tb_c938_2); const char *tb_c938_3 = " = tb_d"; long long tb_c938_3n = sizeof(" = tb_d") - 1; const char *tb_c938_4 = v_id; long long tb_c938_4n = (long long)strlen(tb_c938_4); const char *tb_c938_5 = "[tb_i"; long long tb_c938_5n = sizeof("[tb_i") - 1; const char *tb_c938_6 = v_id; long long tb_c938_6n = (long long)strlen(tb_c938_6); const char *tb_c938_7 = "];\n"; long long tb_c938_7n = sizeof("];\n") - 1; char *tb_c938r = tb_mem(tb_c938_0n + tb_c938_1n + tb_c938_2n + tb_c938_3n + tb_c938_4n + tb_c938_5n + tb_c938_6n + tb_c938_7n + 1), *tb_c938rw = tb_c938r; memcpy(tb_c938rw, tb_c938_0, tb_c938_0n); tb_c938rw += tb_c938_0n; memcpy(tb_c938rw, tb_c938_1, tb_c938_1n); tb_c938rw += tb_c938_1n; memcpy(tb_c938rw, tb_c938_2, tb_c938_2n); tb_c938rw += tb_c938_2n; memcpy(tb_c938rw, tb_c938_3, tb_c938_3n); tb_c938rw += tb_c938_3n; memcpy(tb_c938rw, tb_c938_4, tb_c938_4n); tb_c938rw += tb_c938_4n; memcpy(tb_c938rw, tb_c938_5, tb_c938_5n); tb_c938rw += tb_c938_5n; memcpy(tb_c938rw, tb_c938_6, tb_c938_6n); tb_c938rw += tb_c938_6n; memcpy(tb_c938rw, tb_c938_7, tb_c938_7n); tb_c938rw += tb_c938_7n; *tb_c938rw = 0; (const char *)tb_c938r; }));
    {
        tb_list * tb_l939 = ((tb_o_Node *)tb_nn(v_n, 943))->q_caps;
        for (long long tb_i939 = 0; tb_i939 < tb_l939->len; tb_i939++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l939->data)[tb_i939];
            if (((tb_o_Var *)tb_nn(v_v, 944))->q_is_shared) {
                { const char * tb_v940 = ((tb_o_Var *)tb_nn(v_v, 945))->q_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 945))->q_vec_names) = tb_v940; }
                { const char * tb_v941 = ({ const char *tb_c942_0 = "tb_a"; long long tb_c942_0n = sizeof("tb_a") - 1; const char *tb_c942_1 = v_id; long long tb_c942_1n = (long long)strlen(tb_c942_1); const char *tb_c942_2 = "_"; long long tb_c942_2n = sizeof("_") - 1; const char *tb_c942_3 = ((tb_o_Var *)tb_nn(v_v, 946))->q_name; long long tb_c942_3n = (long long)strlen(tb_c942_3); const char *tb_c942_4 = "_0"; long long tb_c942_4n = sizeof("_0") - 1; char *tb_c942r = tb_mem(tb_c942_0n + tb_c942_1n + tb_c942_2n + tb_c942_3n + tb_c942_4n + 1), *tb_c942rw = tb_c942r; memcpy(tb_c942rw, tb_c942_0, tb_c942_0n); tb_c942rw += tb_c942_0n; memcpy(tb_c942rw, tb_c942_1, tb_c942_1n); tb_c942rw += tb_c942_1n; memcpy(tb_c942rw, tb_c942_2, tb_c942_2n); tb_c942rw += tb_c942_2n; memcpy(tb_c942rw, tb_c942_3, tb_c942_3n); tb_c942rw += tb_c942_3n; memcpy(tb_c942rw, tb_c942_4, tb_c942_4n); tb_c942rw += tb_c942_4n; *tb_c942rw = 0; (const char *)tb_c942r; }); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 946))->q_vec_repl) = tb_v941; }
            }
        }
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 947))->q_kids, 1LL, sizeof(tb_o_Node *), 947)), (v_d + 1LL));
    tb_list * v_fresh_names2 = tb_list_new(sizeof(const char *));
    tb_list * v_fresh_repl2 = tb_list_new(sizeof(const char *));
    { tb_list * tb_v943 = v_fresh_names2; tb_list * *tb_p943 = &(((tb_o_Comp *)tb_nn(v_c, 950))->q_vec_names); *tb_p943 = tb_v943; }
    { tb_list * tb_v944 = v_fresh_repl2; tb_list * *tb_p944 = &(((tb_o_Comp *)tb_nn(v_c, 951))->q_vec_repl); *tb_p944 = tb_v944; }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, "}\n");
    {
        tb_list * tb_l945 = ((tb_o_Node *)tb_nn(v_n, 955))->q_caps;
        for (long long tb_i945 = 0; tb_i945 < tb_l945->len; tb_i945++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l945->data)[tb_i945];
            if (((tb_o_Var *)tb_nn(v_v, 956))->q_is_shared) {
                const char * v_target = ({ const char *tb_c946_0 = "v_"; long long tb_c946_0n = sizeof("v_") - 1; const char *tb_c946_1 = ((tb_o_Var *)tb_nn(v_v, 957))->q_name; long long tb_c946_1n = (long long)strlen(tb_c946_1); char *tb_c946r = tb_mem(tb_c946_0n + tb_c946_1n + 1), *tb_c946rw = tb_c946r; memcpy(tb_c946rw, tb_c946_0, tb_c946_0n); tb_c946rw += tb_c946_0n; memcpy(tb_c946rw, tb_c946_1, tb_c946_1n); tb_c946rw += tb_c946_1n; *tb_c946rw = 0; (const char *)tb_c946r; });
                if (((tb_o_Var *)tb_nn(v_v, 958))->q_is_link) {
                    v_target = ({ const char *tb_c947_0 = "(*v_"; long long tb_c947_0n = sizeof("(*v_") - 1; const char *tb_c947_1 = ((tb_o_Var *)tb_nn(v_v, 959))->q_name; long long tb_c947_1n = (long long)strlen(tb_c947_1); const char *tb_c947_2 = ")"; long long tb_c947_2n = sizeof(")") - 1; char *tb_c947r = tb_mem(tb_c947_0n + tb_c947_1n + tb_c947_2n + 1), *tb_c947rw = tb_c947r; memcpy(tb_c947rw, tb_c947_0, tb_c947_0n); tb_c947rw += tb_c947_0n; memcpy(tb_c947rw, tb_c947_1, tb_c947_1n); tb_c947rw += tb_c947_1n; memcpy(tb_c947rw, tb_c947_2, tb_c947_2n); tb_c947rw += tb_c947_2n; *tb_c947rw = 0; (const char *)tb_c947r; });
                }
                const char * v_a = ({ const char *tb_c948_0 = "tb_a"; long long tb_c948_0n = sizeof("tb_a") - 1; const char *tb_c948_1 = v_id; long long tb_c948_1n = (long long)strlen(tb_c948_1); const char *tb_c948_2 = "_"; long long tb_c948_2n = sizeof("_") - 1; const char *tb_c948_3 = ((tb_o_Var *)tb_nn(v_v, 960))->q_name; long long tb_c948_3n = (long long)strlen(tb_c948_3); const char *tb_c948_4 = "_"; long long tb_c948_4n = sizeof("_") - 1; char *tb_c948r = tb_mem(tb_c948_0n + tb_c948_1n + tb_c948_2n + tb_c948_3n + tb_c948_4n + 1), *tb_c948rw = tb_c948r; memcpy(tb_c948rw, tb_c948_0, tb_c948_0n); tb_c948rw += tb_c948_0n; memcpy(tb_c948rw, tb_c948_1, tb_c948_1n); tb_c948rw += tb_c948_1n; memcpy(tb_c948rw, tb_c948_2, tb_c948_2n); tb_c948rw += tb_c948_2n; memcpy(tb_c948rw, tb_c948_3, tb_c948_3n); tb_c948rw += tb_c948_3n; memcpy(tb_c948rw, tb_c948_4, tb_c948_4n); tb_c948rw += tb_c948_4n; *tb_c948rw = 0; (const char *)tb_c948r; });
                f_emit__indent(v_c, v_d);
                f_emit__put(v_c, ({ const char *tb_c949_0 = v_target; long long tb_c949_0n = (long long)strlen(tb_c949_0); const char *tb_c949_1 = " += (("; long long tb_c949_1n = sizeof(" += ((") - 1; const char *tb_c949_2 = v_a; long long tb_c949_2n = (long long)strlen(tb_c949_2); const char *tb_c949_3 = "0 + "; long long tb_c949_3n = sizeof("0 + ") - 1; const char *tb_c949_4 = v_a; long long tb_c949_4n = (long long)strlen(tb_c949_4); const char *tb_c949_5 = "1) + ("; long long tb_c949_5n = sizeof("1) + (") - 1; const char *tb_c949_6 = v_a; long long tb_c949_6n = (long long)strlen(tb_c949_6); const char *tb_c949_7 = "2 + "; long long tb_c949_7n = sizeof("2 + ") - 1; const char *tb_c949_8 = v_a; long long tb_c949_8n = (long long)strlen(tb_c949_8); const char *tb_c949_9 = "3)) + (("; long long tb_c949_9n = sizeof("3)) + ((") - 1; const char *tb_c949_10 = v_a; long long tb_c949_10n = (long long)strlen(tb_c949_10); const char *tb_c949_11 = "4 + "; long long tb_c949_11n = sizeof("4 + ") - 1; const char *tb_c949_12 = v_a; long long tb_c949_12n = (long long)strlen(tb_c949_12); const char *tb_c949_13 = "5) + ("; long long tb_c949_13n = sizeof("5) + (") - 1; const char *tb_c949_14 = v_a; long long tb_c949_14n = (long long)strlen(tb_c949_14); const char *tb_c949_15 = "6 + "; long long tb_c949_15n = sizeof("6 + ") - 1; const char *tb_c949_16 = v_a; long long tb_c949_16n = (long long)strlen(tb_c949_16); const char *tb_c949_17 = "7));\n"; long long tb_c949_17n = sizeof("7));\n") - 1; char *tb_c949r = tb_mem(tb_c949_0n + tb_c949_1n + tb_c949_2n + tb_c949_3n + tb_c949_4n + tb_c949_5n + tb_c949_6n + tb_c949_7n + tb_c949_8n + tb_c949_9n + tb_c949_10n + tb_c949_11n + tb_c949_12n + tb_c949_13n + tb_c949_14n + tb_c949_15n + tb_c949_16n + tb_c949_17n + 1), *tb_c949rw = tb_c949r; memcpy(tb_c949rw, tb_c949_0, tb_c949_0n); tb_c949rw += tb_c949_0n; memcpy(tb_c949rw, tb_c949_1, tb_c949_1n); tb_c949rw += tb_c949_1n; memcpy(tb_c949rw, tb_c949_2, tb_c949_2n); tb_c949rw += tb_c949_2n; memcpy(tb_c949rw, tb_c949_3, tb_c949_3n); tb_c949rw += tb_c949_3n; memcpy(tb_c949rw, tb_c949_4, tb_c949_4n); tb_c949rw += tb_c949_4n; memcpy(tb_c949rw, tb_c949_5, tb_c949_5n); tb_c949rw += tb_c949_5n; memcpy(tb_c949rw, tb_c949_6, tb_c949_6n); tb_c949rw += tb_c949_6n; memcpy(tb_c949rw, tb_c949_7, tb_c949_7n); tb_c949rw += tb_c949_7n; memcpy(tb_c949rw, tb_c949_8, tb_c949_8n); tb_c949rw += tb_c949_8n; memcpy(tb_c949rw, tb_c949_9, tb_c949_9n); tb_c949rw += tb_c949_9n; memcpy(tb_c949rw, tb_c949_10, tb_c949_10n); tb_c949rw += tb_c949_10n; memcpy(tb_c949rw, tb_c949_11, tb_c949_11n); tb_c949rw += tb_c949_11n; memcpy(tb_c949rw, tb_c949_12, tb_c949_12n); tb_c949rw += tb_c949_12n; memcpy(tb_c949rw, tb_c949_13, tb_c949_13n); tb_c949rw += tb_c949_13n; memcpy(tb_c949rw, tb_c949_14, tb_c949_14n); tb_c949rw += tb_c949_14n; memcpy(tb_c949rw, tb_c949_15, tb_c949_15n); tb_c949rw += tb_c949_15n; memcpy(tb_c949rw, tb_c949_16, tb_c949_16n); tb_c949rw += tb_c949_16n; memcpy(tb_c949rw, tb_c949_17, tb_c949_17n); tb_c949rw += tb_c949_17n; *tb_c949rw = 0; (const char *)tb_c949r; }));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_each(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    if (((tb_o_Node *)tb_nn(v_n, 967))->q_by_vec) {
        f_emit__emit_vec(v_c, v_n, v_depth);
        return;
    }
    if (((tb_o_Node *)tb_nn(v_n, 970))->q_is_par) {
        f_emit__emit_par(v_c, v_n, v_depth);
        return;
    }
    if (((tb_o_Node *)tb_nn(v_n, 973))->q_by_piece) {
        f_emit__emit_pieces(v_c, v_n, v_depth);
        return;
    }
    { long long tb_v950 = 1LL; long long *tb_p950 = &(((tb_o_Comp *)tb_nn(v_c, 976))->q_label); *tb_p950 += tb_v950; }
    const char * v_id = ({ const char *tb_c951_0 = ""; long long tb_c951_0n = sizeof("") - 1; char tb_c951_1d[24]; long long tb_c951_1n; const char *tb_c951_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 977))->q_label, tb_c951_1d, &tb_c951_1n); char *tb_c951r = tb_mem(tb_c951_0n + tb_c951_1n + 1), *tb_c951rw = tb_c951r; memcpy(tb_c951rw, tb_c951_0, tb_c951_0n); tb_c951rw += tb_c951_0n; memcpy(tb_c951rw, tb_c951_1, tb_c951_1n); tb_c951rw += tb_c951_1n; *tb_c951rw = 0; (const char *)tb_c951r; });
    long long v_t = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 978))->q_kids, 0LL, sizeof(tb_o_Node *), 978)), 978))->q_ty;
    const char * v_k = f_check__kind_of(v_c, v_t);
    long long v_elem_ty = (-1LL);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    if (({ const char *tb_s952 = v_k; ((unsigned char)tb_s952[0] == 100 && (unsigned char)tb_s952[1] == 105 && (unsigned char)tb_s952[2] == 99 && (unsigned char)tb_s952[3] == 116 && tb_s952[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c953_0 = "tb_list * tb_l"; long long tb_c953_0n = sizeof("tb_list * tb_l") - 1; const char *tb_c953_1 = v_id; long long tb_c953_1n = (long long)strlen(tb_c953_1); const char *tb_c953_2 = " = ("; long long tb_c953_2n = sizeof(" = (") - 1; char *tb_c953r = tb_mem(tb_c953_0n + tb_c953_1n + tb_c953_2n + 1), *tb_c953rw = tb_c953r; memcpy(tb_c953rw, tb_c953_0, tb_c953_0n); tb_c953rw += tb_c953_0n; memcpy(tb_c953rw, tb_c953_1, tb_c953_1n); tb_c953rw += tb_c953_1n; memcpy(tb_c953rw, tb_c953_2, tb_c953_2n); tb_c953rw += tb_c953_2n; *tb_c953rw = 0; (const char *)tb_c953r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 986))->q_kids, 0LL, sizeof(tb_o_Node *), 986)));
        f_emit__put(v_c, ")->keys;\n");
        v_k = "list";
        v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 989))->q_types, v_t, sizeof(tb_o_Type *), 989)), 989))->q_key;
    } else {
        f_emit__put(v_c, ({ const char *tb_c954_0 = f_emit__c_type(v_c, v_t); long long tb_c954_0n = (long long)strlen(tb_c954_0); const char *tb_c954_1 = " tb_l"; long long tb_c954_1n = sizeof(" tb_l") - 1; const char *tb_c954_2 = v_id; long long tb_c954_2n = (long long)strlen(tb_c954_2); const char *tb_c954_3 = " = "; long long tb_c954_3n = sizeof(" = ") - 1; char *tb_c954r = tb_mem(tb_c954_0n + tb_c954_1n + tb_c954_2n + tb_c954_3n + 1), *tb_c954rw = tb_c954r; memcpy(tb_c954rw, tb_c954_0, tb_c954_0n); tb_c954rw += tb_c954_0n; memcpy(tb_c954rw, tb_c954_1, tb_c954_1n); tb_c954rw += tb_c954_1n; memcpy(tb_c954rw, tb_c954_2, tb_c954_2n); tb_c954rw += tb_c954_2n; memcpy(tb_c954rw, tb_c954_3, tb_c954_3n); tb_c954rw += tb_c954_3n; *tb_c954rw = 0; (const char *)tb_c954r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 992))->q_kids, 0LL, sizeof(tb_o_Node *), 992)));
        f_emit__put(v_c, ";\n");
        if (({ const char *tb_s955 = v_k; ((unsigned char)tb_s955[0] == 108 && (unsigned char)tb_s955[1] == 105 && (unsigned char)tb_s955[2] == 115 && (unsigned char)tb_s955[3] == 116 && tb_s955[4] == 0); })) {
            v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 995))->q_types, v_t, sizeof(tb_o_Type *), 995)), 995))->q_elem;
        }
    }
    f_emit__indent(v_c, (v_depth + 1LL));
    if (({ const char *tb_s956 = v_k; ((unsigned char)tb_s956[0] == 116 && (unsigned char)tb_s956[1] == 101 && (unsigned char)tb_s956[2] == 120 && (unsigned char)tb_s956[3] == 116 && tb_s956[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c957_0 = "for (long long tb_i"; long long tb_c957_0n = sizeof("for (long long tb_i") - 1; const char *tb_c957_1 = v_id; long long tb_c957_1n = (long long)strlen(tb_c957_1); const char *tb_c957_2 = " = 0; tb_l"; long long tb_c957_2n = sizeof(" = 0; tb_l") - 1; const char *tb_c957_3 = v_id; long long tb_c957_3n = (long long)strlen(tb_c957_3); const char *tb_c957_4 = "[tb_i"; long long tb_c957_4n = sizeof("[tb_i") - 1; const char *tb_c957_5 = v_id; long long tb_c957_5n = (long long)strlen(tb_c957_5); const char *tb_c957_6 = "] != 0; tb_i"; long long tb_c957_6n = sizeof("] != 0; tb_i") - 1; const char *tb_c957_7 = v_id; long long tb_c957_7n = (long long)strlen(tb_c957_7); const char *tb_c957_8 = "++) {\n"; long long tb_c957_8n = sizeof("++) {\n") - 1; char *tb_c957r = tb_mem(tb_c957_0n + tb_c957_1n + tb_c957_2n + tb_c957_3n + tb_c957_4n + tb_c957_5n + tb_c957_6n + tb_c957_7n + tb_c957_8n + 1), *tb_c957rw = tb_c957r; memcpy(tb_c957rw, tb_c957_0, tb_c957_0n); tb_c957rw += tb_c957_0n; memcpy(tb_c957rw, tb_c957_1, tb_c957_1n); tb_c957rw += tb_c957_1n; memcpy(tb_c957rw, tb_c957_2, tb_c957_2n); tb_c957rw += tb_c957_2n; memcpy(tb_c957rw, tb_c957_3, tb_c957_3n); tb_c957rw += tb_c957_3n; memcpy(tb_c957rw, tb_c957_4, tb_c957_4n); tb_c957rw += tb_c957_4n; memcpy(tb_c957rw, tb_c957_5, tb_c957_5n); tb_c957rw += tb_c957_5n; memcpy(tb_c957rw, tb_c957_6, tb_c957_6n); tb_c957rw += tb_c957_6n; memcpy(tb_c957rw, tb_c957_7, tb_c957_7n); tb_c957rw += tb_c957_7n; memcpy(tb_c957rw, tb_c957_8, tb_c957_8n); tb_c957rw += tb_c957_8n; *tb_c957rw = 0; (const char *)tb_c957r; }));
        f_emit__indent(v_c, (v_depth + 2LL));
        f_emit__put(v_c, ({ const char *tb_c958_0 = "const char * const v_"; long long tb_c958_0n = sizeof("const char * const v_") - 1; const char *tb_c958_1 = ((tb_o_Node *)tb_nn(v_n, 1000))->q_name; long long tb_c958_1n = (long long)strlen(tb_c958_1); const char *tb_c958_2 = " = tb_chr((unsigned char)tb_l"; long long tb_c958_2n = sizeof(" = tb_chr((unsigned char)tb_l") - 1; const char *tb_c958_3 = v_id; long long tb_c958_3n = (long long)strlen(tb_c958_3); const char *tb_c958_4 = "[tb_i"; long long tb_c958_4n = sizeof("[tb_i") - 1; const char *tb_c958_5 = v_id; long long tb_c958_5n = (long long)strlen(tb_c958_5); const char *tb_c958_6 = "]);\n"; long long tb_c958_6n = sizeof("]);\n") - 1; char *tb_c958r = tb_mem(tb_c958_0n + tb_c958_1n + tb_c958_2n + tb_c958_3n + tb_c958_4n + tb_c958_5n + tb_c958_6n + 1), *tb_c958rw = tb_c958r; memcpy(tb_c958rw, tb_c958_0, tb_c958_0n); tb_c958rw += tb_c958_0n; memcpy(tb_c958rw, tb_c958_1, tb_c958_1n); tb_c958rw += tb_c958_1n; memcpy(tb_c958rw, tb_c958_2, tb_c958_2n); tb_c958rw += tb_c958_2n; memcpy(tb_c958rw, tb_c958_3, tb_c958_3n); tb_c958rw += tb_c958_3n; memcpy(tb_c958rw, tb_c958_4, tb_c958_4n); tb_c958rw += tb_c958_4n; memcpy(tb_c958rw, tb_c958_5, tb_c958_5n); tb_c958rw += tb_c958_5n; memcpy(tb_c958rw, tb_c958_6, tb_c958_6n); tb_c958rw += tb_c958_6n; *tb_c958rw = 0; (const char *)tb_c958r; }));
    } else {
        const char * v_elem = f_emit__c_type(v_c, v_elem_ty);
        f_emit__put(v_c, ({ const char *tb_c959_0 = "for (long long tb_i"; long long tb_c959_0n = sizeof("for (long long tb_i") - 1; const char *tb_c959_1 = v_id; long long tb_c959_1n = (long long)strlen(tb_c959_1); const char *tb_c959_2 = " = 0; tb_i"; long long tb_c959_2n = sizeof(" = 0; tb_i") - 1; const char *tb_c959_3 = v_id; long long tb_c959_3n = (long long)strlen(tb_c959_3); const char *tb_c959_4 = " < tb_l"; long long tb_c959_4n = sizeof(" < tb_l") - 1; const char *tb_c959_5 = v_id; long long tb_c959_5n = (long long)strlen(tb_c959_5); const char *tb_c959_6 = "->len; tb_i"; long long tb_c959_6n = sizeof("->len; tb_i") - 1; const char *tb_c959_7 = v_id; long long tb_c959_7n = (long long)strlen(tb_c959_7); const char *tb_c959_8 = "++) {\n"; long long tb_c959_8n = sizeof("++) {\n") - 1; char *tb_c959r = tb_mem(tb_c959_0n + tb_c959_1n + tb_c959_2n + tb_c959_3n + tb_c959_4n + tb_c959_5n + tb_c959_6n + tb_c959_7n + tb_c959_8n + 1), *tb_c959rw = tb_c959r; memcpy(tb_c959rw, tb_c959_0, tb_c959_0n); tb_c959rw += tb_c959_0n; memcpy(tb_c959rw, tb_c959_1, tb_c959_1n); tb_c959rw += tb_c959_1n; memcpy(tb_c959rw, tb_c959_2, tb_c959_2n); tb_c959rw += tb_c959_2n; memcpy(tb_c959rw, tb_c959_3, tb_c959_3n); tb_c959rw += tb_c959_3n; memcpy(tb_c959rw, tb_c959_4, tb_c959_4n); tb_c959rw += tb_c959_4n; memcpy(tb_c959rw, tb_c959_5, tb_c959_5n); tb_c959rw += tb_c959_5n; memcpy(tb_c959rw, tb_c959_6, tb_c959_6n); tb_c959rw += tb_c959_6n; memcpy(tb_c959rw, tb_c959_7, tb_c959_7n); tb_c959rw += tb_c959_7n; memcpy(tb_c959rw, tb_c959_8, tb_c959_8n); tb_c959rw += tb_c959_8n; *tb_c959rw = 0; (const char *)tb_c959r; }));
        f_emit__indent(v_c, (v_depth + 2LL));
        if (f_check__is_cols_obj(v_c, v_elem_ty)) {
            f_emit__put(v_c, ({ const char *tb_c960_0 = v_elem; long long tb_c960_0n = (long long)strlen(tb_c960_0); const char *tb_c960_1 = " const v_"; long long tb_c960_1n = sizeof(" const v_") - 1; const char *tb_c960_2 = ((tb_o_Node *)tb_nn(v_n, 1006))->q_name; long long tb_c960_2n = (long long)strlen(tb_c960_2); const char *tb_c960_3 = " = {tb_l"; long long tb_c960_3n = sizeof(" = {tb_l") - 1; const char *tb_c960_4 = v_id; long long tb_c960_4n = (long long)strlen(tb_c960_4); const char *tb_c960_5 = ", tb_i"; long long tb_c960_5n = sizeof(", tb_i") - 1; const char *tb_c960_6 = v_id; long long tb_c960_6n = (long long)strlen(tb_c960_6); const char *tb_c960_7 = "};\n"; long long tb_c960_7n = sizeof("};\n") - 1; char *tb_c960r = tb_mem(tb_c960_0n + tb_c960_1n + tb_c960_2n + tb_c960_3n + tb_c960_4n + tb_c960_5n + tb_c960_6n + tb_c960_7n + 1), *tb_c960rw = tb_c960r; memcpy(tb_c960rw, tb_c960_0, tb_c960_0n); tb_c960rw += tb_c960_0n; memcpy(tb_c960rw, tb_c960_1, tb_c960_1n); tb_c960rw += tb_c960_1n; memcpy(tb_c960rw, tb_c960_2, tb_c960_2n); tb_c960rw += tb_c960_2n; memcpy(tb_c960rw, tb_c960_3, tb_c960_3n); tb_c960rw += tb_c960_3n; memcpy(tb_c960rw, tb_c960_4, tb_c960_4n); tb_c960rw += tb_c960_4n; memcpy(tb_c960rw, tb_c960_5, tb_c960_5n); tb_c960rw += tb_c960_5n; memcpy(tb_c960rw, tb_c960_6, tb_c960_6n); tb_c960rw += tb_c960_6n; memcpy(tb_c960rw, tb_c960_7, tb_c960_7n); tb_c960rw += tb_c960_7n; *tb_c960rw = 0; (const char *)tb_c960r; }));
        } else if ((v_elem_ty == 3LL)) {
            f_emit__put(v_c, ({ const char *tb_c961_0 = "_Bool const v_"; long long tb_c961_0n = sizeof("_Bool const v_") - 1; const char *tb_c961_1 = ((tb_o_Node *)tb_nn(v_n, 1008))->q_name; long long tb_c961_1n = (long long)strlen(tb_c961_1); const char *tb_c961_2 = " = TB_BIT(tb_l"; long long tb_c961_2n = sizeof(" = TB_BIT(tb_l") - 1; const char *tb_c961_3 = v_id; long long tb_c961_3n = (long long)strlen(tb_c961_3); const char *tb_c961_4 = ", tb_i"; long long tb_c961_4n = sizeof(", tb_i") - 1; const char *tb_c961_5 = v_id; long long tb_c961_5n = (long long)strlen(tb_c961_5); const char *tb_c961_6 = ");\n"; long long tb_c961_6n = sizeof(");\n") - 1; char *tb_c961r = tb_mem(tb_c961_0n + tb_c961_1n + tb_c961_2n + tb_c961_3n + tb_c961_4n + tb_c961_5n + tb_c961_6n + 1), *tb_c961rw = tb_c961r; memcpy(tb_c961rw, tb_c961_0, tb_c961_0n); tb_c961rw += tb_c961_0n; memcpy(tb_c961rw, tb_c961_1, tb_c961_1n); tb_c961rw += tb_c961_1n; memcpy(tb_c961rw, tb_c961_2, tb_c961_2n); tb_c961rw += tb_c961_2n; memcpy(tb_c961rw, tb_c961_3, tb_c961_3n); tb_c961rw += tb_c961_3n; memcpy(tb_c961rw, tb_c961_4, tb_c961_4n); tb_c961rw += tb_c961_4n; memcpy(tb_c961rw, tb_c961_5, tb_c961_5n); tb_c961rw += tb_c961_5n; memcpy(tb_c961rw, tb_c961_6, tb_c961_6n); tb_c961rw += tb_c961_6n; *tb_c961rw = 0; (const char *)tb_c961r; }));
        } else {
            f_emit__put(v_c, ({ const char *tb_c962_0 = v_elem; long long tb_c962_0n = (long long)strlen(tb_c962_0); const char *tb_c962_1 = " const v_"; long long tb_c962_1n = sizeof(" const v_") - 1; const char *tb_c962_2 = ((tb_o_Node *)tb_nn(v_n, 1010))->q_name; long long tb_c962_2n = (long long)strlen(tb_c962_2); const char *tb_c962_3 = " = (("; long long tb_c962_3n = sizeof(" = ((") - 1; const char *tb_c962_4 = v_elem; long long tb_c962_4n = (long long)strlen(tb_c962_4); const char *tb_c962_5 = " *)tb_l"; long long tb_c962_5n = sizeof(" *)tb_l") - 1; const char *tb_c962_6 = v_id; long long tb_c962_6n = (long long)strlen(tb_c962_6); const char *tb_c962_7 = "->data)[tb_i"; long long tb_c962_7n = sizeof("->data)[tb_i") - 1; const char *tb_c962_8 = v_id; long long tb_c962_8n = (long long)strlen(tb_c962_8); const char *tb_c962_9 = "];\n"; long long tb_c962_9n = sizeof("];\n") - 1; char *tb_c962r = tb_mem(tb_c962_0n + tb_c962_1n + tb_c962_2n + tb_c962_3n + tb_c962_4n + tb_c962_5n + tb_c962_6n + tb_c962_7n + tb_c962_8n + tb_c962_9n + 1), *tb_c962rw = tb_c962r; memcpy(tb_c962rw, tb_c962_0, tb_c962_0n); tb_c962rw += tb_c962_0n; memcpy(tb_c962rw, tb_c962_1, tb_c962_1n); tb_c962rw += tb_c962_1n; memcpy(tb_c962rw, tb_c962_2, tb_c962_2n); tb_c962rw += tb_c962_2n; memcpy(tb_c962rw, tb_c962_3, tb_c962_3n); tb_c962rw += tb_c962_3n; memcpy(tb_c962rw, tb_c962_4, tb_c962_4n); tb_c962rw += tb_c962_4n; memcpy(tb_c962rw, tb_c962_5, tb_c962_5n); tb_c962rw += tb_c962_5n; memcpy(tb_c962rw, tb_c962_6, tb_c962_6n); tb_c962rw += tb_c962_6n; memcpy(tb_c962rw, tb_c962_7, tb_c962_7n); tb_c962rw += tb_c962_7n; memcpy(tb_c962rw, tb_c962_8, tb_c962_8n); tb_c962rw += tb_c962_8n; memcpy(tb_c962rw, tb_c962_9, tb_c962_9n); tb_c962rw += tb_c962_9n; *tb_c962rw = 0; (const char *)tb_c962r; }));
            f_emit__emit_prefetch(v_c, v_n, (v_depth + 2LL), ({ const char *tb_c963_0 = "tb_l"; long long tb_c963_0n = sizeof("tb_l") - 1; const char *tb_c963_1 = v_id; long long tb_c963_1n = (long long)strlen(tb_c963_1); char *tb_c963r = tb_mem(tb_c963_0n + tb_c963_1n + 1), *tb_c963rw = tb_c963r; memcpy(tb_c963rw, tb_c963_0, tb_c963_0n); tb_c963rw += tb_c963_0n; memcpy(tb_c963rw, tb_c963_1, tb_c963_1n); tb_c963rw += tb_c963_1n; *tb_c963rw = 0; (const char *)tb_c963r; }), ({ const char *tb_c964_0 = "tb_i"; long long tb_c964_0n = sizeof("tb_i") - 1; const char *tb_c964_1 = v_id; long long tb_c964_1n = (long long)strlen(tb_c964_1); char *tb_c964r = tb_mem(tb_c964_0n + tb_c964_1n + 1), *tb_c964rw = tb_c964r; memcpy(tb_c964rw, tb_c964_0, tb_c964_0n); tb_c964rw += tb_c964_0n; memcpy(tb_c964rw, tb_c964_1, tb_c964_1n); tb_c964rw += tb_c964_1n; *tb_c964rw = 0; (const char *)tb_c964r; }), v_elem);
        }
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1012))->q_kids, 1LL, sizeof(tb_o_Node *), 1012)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_pieces(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v965 = 1LL; long long *tb_p965 = &(((tb_o_Comp *)tb_nn(v_c, 1023))->q_label); *tb_p965 += tb_v965; }
    const char * v_id = ({ const char *tb_c966_0 = ""; long long tb_c966_0n = sizeof("") - 1; char tb_c966_1d[24]; long long tb_c966_1n; const char *tb_c966_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1024))->q_label, tb_c966_1d, &tb_c966_1n); char *tb_c966r = tb_mem(tb_c966_0n + tb_c966_1n + 1), *tb_c966rw = tb_c966r; memcpy(tb_c966rw, tb_c966_0, tb_c966_0n); tb_c966rw += tb_c966_0n; memcpy(tb_c966rw, tb_c966_1, tb_c966_1n); tb_c966rw += tb_c966_1n; *tb_c966rw = 0; (const char *)tb_c966r; });
    const char * v_l = ({ const char *tb_c967_0 = "tb_l"; long long tb_c967_0n = sizeof("tb_l") - 1; const char *tb_c967_1 = v_id; long long tb_c967_1n = (long long)strlen(tb_c967_1); char *tb_c967r = tb_mem(tb_c967_0n + tb_c967_1n + 1), *tb_c967rw = tb_c967r; memcpy(tb_c967rw, tb_c967_0, tb_c967_0n); tb_c967rw += tb_c967_0n; memcpy(tb_c967rw, tb_c967_1, tb_c967_1n); tb_c967rw += tb_c967_1n; *tb_c967rw = 0; (const char *)tb_c967r; });
    const char * v_i = ({ const char *tb_c968_0 = "tb_i"; long long tb_c968_0n = sizeof("tb_i") - 1; const char *tb_c968_1 = v_id; long long tb_c968_1n = (long long)strlen(tb_c968_1); char *tb_c968r = tb_mem(tb_c968_0n + tb_c968_1n + 1), *tb_c968rw = tb_c968r; memcpy(tb_c968rw, tb_c968_0, tb_c968_0n); tb_c968rw += tb_c968_0n; memcpy(tb_c968rw, tb_c968_1, tb_c968_1n); tb_c968rw += tb_c968_1n; *tb_c968rw = 0; (const char *)tb_c968r; });
    const char * v_p = ({ const char *tb_c969_0 = "tb_p"; long long tb_c969_0n = sizeof("tb_p") - 1; const char *tb_c969_1 = v_id; long long tb_c969_1n = (long long)strlen(tb_c969_1); char *tb_c969r = tb_mem(tb_c969_0n + tb_c969_1n + 1), *tb_c969rw = tb_c969r; memcpy(tb_c969rw, tb_c969_0, tb_c969_0n); tb_c969rw += tb_c969_0n; memcpy(tb_c969rw, tb_c969_1, tb_c969_1n); tb_c969rw += tb_c969_1n; *tb_c969rw = 0; (const char *)tb_c969r; });
    const char * v_m = ({ const char *tb_c970_0 = "tb_n"; long long tb_c970_0n = sizeof("tb_n") - 1; const char *tb_c970_1 = v_id; long long tb_c970_1n = (long long)strlen(tb_c970_1); char *tb_c970r = tb_mem(tb_c970_0n + tb_c970_1n + 1), *tb_c970rw = tb_c970r; memcpy(tb_c970rw, tb_c970_0, tb_c970_0n); tb_c970rw += tb_c970_0n; memcpy(tb_c970rw, tb_c970_1, tb_c970_1n); tb_c970rw += tb_c970_1n; *tb_c970rw = 0; (const char *)tb_c970r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c971_0 = "tb_list *"; long long tb_c971_0n = sizeof("tb_list *") - 1; const char *tb_c971_1 = v_l; long long tb_c971_1n = (long long)strlen(tb_c971_1); const char *tb_c971_2 = " = "; long long tb_c971_2n = sizeof(" = ") - 1; char *tb_c971r = tb_mem(tb_c971_0n + tb_c971_1n + tb_c971_2n + 1), *tb_c971rw = tb_c971r; memcpy(tb_c971rw, tb_c971_0, tb_c971_0n); tb_c971rw += tb_c971_0n; memcpy(tb_c971rw, tb_c971_1, tb_c971_1n); tb_c971rw += tb_c971_1n; memcpy(tb_c971rw, tb_c971_2, tb_c971_2n); tb_c971rw += tb_c971_2n; *tb_c971rw = 0; (const char *)tb_c971r; }));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1033))->q_kids, 0LL, sizeof(tb_o_Node *), 1033)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c972_0 = "long long "; long long tb_c972_0n = sizeof("long long ") - 1; const char *tb_c972_1 = v_m; long long tb_c972_1n = (long long)strlen(tb_c972_1); const char *tb_c972_2 = " = "; long long tb_c972_2n = sizeof(" = ") - 1; char *tb_c972r = tb_mem(tb_c972_0n + tb_c972_1n + tb_c972_2n + 1), *tb_c972rw = tb_c972r; memcpy(tb_c972rw, tb_c972_0, tb_c972_0n); tb_c972rw += tb_c972_0n; memcpy(tb_c972rw, tb_c972_1, tb_c972_1n); tb_c972rw += tb_c972_1n; memcpy(tb_c972rw, tb_c972_2, tb_c972_2n); tb_c972rw += tb_c972_2n; *tb_c972rw = 0; (const char *)tb_c972r; }));
    f_emit__emit_expr(v_c, ((tb_o_Node *)tb_nn(v_n, 1037))->q_piece_size);
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c973_0 = "if ("; long long tb_c973_0n = sizeof("if (") - 1; const char *tb_c973_1 = v_m; long long tb_c973_1n = (long long)strlen(tb_c973_1); const char *tb_c973_2 = " <= 0) tb_crash("; long long tb_c973_2n = sizeof(" <= 0) tb_crash(") - 1; char tb_c973_3d[24]; long long tb_c973_3n; const char *tb_c973_3 = tb_digits(((tb_o_Node *)tb_nn(v_n, 1040))->q_line, tb_c973_3d, &tb_c973_3n); const char *tb_c973_4 = ", \"размер куска в by должен быть больше 0\");\n"; long long tb_c973_4n = sizeof(", \"размер куска в by должен быть больше 0\");\n") - 1; char *tb_c973r = tb_mem(tb_c973_0n + tb_c973_1n + tb_c973_2n + tb_c973_3n + tb_c973_4n + 1), *tb_c973rw = tb_c973r; memcpy(tb_c973rw, tb_c973_0, tb_c973_0n); tb_c973rw += tb_c973_0n; memcpy(tb_c973rw, tb_c973_1, tb_c973_1n); tb_c973rw += tb_c973_1n; memcpy(tb_c973rw, tb_c973_2, tb_c973_2n); tb_c973rw += tb_c973_2n; memcpy(tb_c973rw, tb_c973_3, tb_c973_3n); tb_c973rw += tb_c973_3n; memcpy(tb_c973rw, tb_c973_4, tb_c973_4n); tb_c973rw += tb_c973_4n; *tb_c973rw = 0; (const char *)tb_c973r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c974_0 = "tb_list "; long long tb_c974_0n = sizeof("tb_list ") - 1; const char *tb_c974_1 = v_p; long long tb_c974_1n = (long long)strlen(tb_c974_1); const char *tb_c974_2 = " = {0, 0, -1, "; long long tb_c974_2n = sizeof(" = {0, 0, -1, ") - 1; const char *tb_c974_3 = v_l; long long tb_c974_3n = (long long)strlen(tb_c974_3); const char *tb_c974_4 = "->size};\n"; long long tb_c974_4n = sizeof("->size};\n") - 1; char *tb_c974r = tb_mem(tb_c974_0n + tb_c974_1n + tb_c974_2n + tb_c974_3n + tb_c974_4n + 1), *tb_c974rw = tb_c974r; memcpy(tb_c974rw, tb_c974_0, tb_c974_0n); tb_c974rw += tb_c974_0n; memcpy(tb_c974rw, tb_c974_1, tb_c974_1n); tb_c974rw += tb_c974_1n; memcpy(tb_c974rw, tb_c974_2, tb_c974_2n); tb_c974rw += tb_c974_2n; memcpy(tb_c974rw, tb_c974_3, tb_c974_3n); tb_c974rw += tb_c974_3n; memcpy(tb_c974rw, tb_c974_4, tb_c974_4n); tb_c974rw += tb_c974_4n; *tb_c974rw = 0; (const char *)tb_c974r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c975_0 = "long long tb_cap"; long long tb_c975_0n = sizeof("long long tb_cap") - 1; const char *tb_c975_1 = v_id; long long tb_c975_1n = (long long)strlen(tb_c975_1); const char *tb_c975_2 = " = "; long long tb_c975_2n = sizeof(" = ") - 1; const char *tb_c975_3 = v_l; long long tb_c975_3n = (long long)strlen(tb_c975_3); const char *tb_c975_4 = "->cap;\n"; long long tb_c975_4n = sizeof("->cap;\n") - 1; char *tb_c975r = tb_mem(tb_c975_0n + tb_c975_1n + tb_c975_2n + tb_c975_3n + tb_c975_4n + 1), *tb_c975rw = tb_c975r; memcpy(tb_c975rw, tb_c975_0, tb_c975_0n); tb_c975rw += tb_c975_0n; memcpy(tb_c975rw, tb_c975_1, tb_c975_1n); tb_c975rw += tb_c975_1n; memcpy(tb_c975rw, tb_c975_2, tb_c975_2n); tb_c975rw += tb_c975_2n; memcpy(tb_c975rw, tb_c975_3, tb_c975_3n); tb_c975rw += tb_c975_3n; memcpy(tb_c975rw, tb_c975_4, tb_c975_4n); tb_c975rw += tb_c975_4n; *tb_c975rw = 0; (const char *)tb_c975r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c976_0 = "if ("; long long tb_c976_0n = sizeof("if (") - 1; const char *tb_c976_1 = v_l; long long tb_c976_1n = (long long)strlen(tb_c976_1); const char *tb_c976_2 = "->cap >= 0) "; long long tb_c976_2n = sizeof("->cap >= 0) ") - 1; const char *tb_c976_3 = v_l; long long tb_c976_3n = (long long)strlen(tb_c976_3); const char *tb_c976_4 = "->cap = -2;\n"; long long tb_c976_4n = sizeof("->cap = -2;\n") - 1; char *tb_c976r = tb_mem(tb_c976_0n + tb_c976_1n + tb_c976_2n + tb_c976_3n + tb_c976_4n + 1), *tb_c976rw = tb_c976r; memcpy(tb_c976rw, tb_c976_0, tb_c976_0n); tb_c976rw += tb_c976_0n; memcpy(tb_c976rw, tb_c976_1, tb_c976_1n); tb_c976rw += tb_c976_1n; memcpy(tb_c976rw, tb_c976_2, tb_c976_2n); tb_c976rw += tb_c976_2n; memcpy(tb_c976rw, tb_c976_3, tb_c976_3n); tb_c976rw += tb_c976_3n; memcpy(tb_c976rw, tb_c976_4, tb_c976_4n); tb_c976rw += tb_c976_4n; *tb_c976rw = 0; (const char *)tb_c976r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c977_0 = "for (long long "; long long tb_c977_0n = sizeof("for (long long ") - 1; const char *tb_c977_1 = v_i; long long tb_c977_1n = (long long)strlen(tb_c977_1); const char *tb_c977_2 = " = 0; "; long long tb_c977_2n = sizeof(" = 0; ") - 1; const char *tb_c977_3 = v_i; long long tb_c977_3n = (long long)strlen(tb_c977_3); const char *tb_c977_4 = " < "; long long tb_c977_4n = sizeof(" < ") - 1; const char *tb_c977_5 = v_l; long long tb_c977_5n = (long long)strlen(tb_c977_5); const char *tb_c977_6 = "->len; "; long long tb_c977_6n = sizeof("->len; ") - 1; const char *tb_c977_7 = v_i; long long tb_c977_7n = (long long)strlen(tb_c977_7); const char *tb_c977_8 = " += "; long long tb_c977_8n = sizeof(" += ") - 1; const char *tb_c977_9 = v_m; long long tb_c977_9n = (long long)strlen(tb_c977_9); const char *tb_c977_10 = ") {\n"; long long tb_c977_10n = sizeof(") {\n") - 1; char *tb_c977r = tb_mem(tb_c977_0n + tb_c977_1n + tb_c977_2n + tb_c977_3n + tb_c977_4n + tb_c977_5n + tb_c977_6n + tb_c977_7n + tb_c977_8n + tb_c977_9n + tb_c977_10n + 1), *tb_c977rw = tb_c977r; memcpy(tb_c977rw, tb_c977_0, tb_c977_0n); tb_c977rw += tb_c977_0n; memcpy(tb_c977rw, tb_c977_1, tb_c977_1n); tb_c977rw += tb_c977_1n; memcpy(tb_c977rw, tb_c977_2, tb_c977_2n); tb_c977rw += tb_c977_2n; memcpy(tb_c977rw, tb_c977_3, tb_c977_3n); tb_c977rw += tb_c977_3n; memcpy(tb_c977rw, tb_c977_4, tb_c977_4n); tb_c977rw += tb_c977_4n; memcpy(tb_c977rw, tb_c977_5, tb_c977_5n); tb_c977rw += tb_c977_5n; memcpy(tb_c977rw, tb_c977_6, tb_c977_6n); tb_c977rw += tb_c977_6n; memcpy(tb_c977rw, tb_c977_7, tb_c977_7n); tb_c977rw += tb_c977_7n; memcpy(tb_c977rw, tb_c977_8, tb_c977_8n); tb_c977rw += tb_c977_8n; memcpy(tb_c977rw, tb_c977_9, tb_c977_9n); tb_c977rw += tb_c977_9n; memcpy(tb_c977rw, tb_c977_10, tb_c977_10n); tb_c977rw += tb_c977_10n; *tb_c977rw = 0; (const char *)tb_c977r; }));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, ({ const char *tb_c978_0 = v_p; long long tb_c978_0n = (long long)strlen(tb_c978_0); const char *tb_c978_1 = ".data = "; long long tb_c978_1n = sizeof(".data = ") - 1; const char *tb_c978_2 = v_l; long long tb_c978_2n = (long long)strlen(tb_c978_2); const char *tb_c978_3 = "->data + "; long long tb_c978_3n = sizeof("->data + ") - 1; const char *tb_c978_4 = v_i; long long tb_c978_4n = (long long)strlen(tb_c978_4); const char *tb_c978_5 = " * "; long long tb_c978_5n = sizeof(" * ") - 1; const char *tb_c978_6 = v_l; long long tb_c978_6n = (long long)strlen(tb_c978_6); const char *tb_c978_7 = "->size;\n"; long long tb_c978_7n = sizeof("->size;\n") - 1; char *tb_c978r = tb_mem(tb_c978_0n + tb_c978_1n + tb_c978_2n + tb_c978_3n + tb_c978_4n + tb_c978_5n + tb_c978_6n + tb_c978_7n + 1), *tb_c978rw = tb_c978r; memcpy(tb_c978rw, tb_c978_0, tb_c978_0n); tb_c978rw += tb_c978_0n; memcpy(tb_c978rw, tb_c978_1, tb_c978_1n); tb_c978rw += tb_c978_1n; memcpy(tb_c978rw, tb_c978_2, tb_c978_2n); tb_c978rw += tb_c978_2n; memcpy(tb_c978rw, tb_c978_3, tb_c978_3n); tb_c978rw += tb_c978_3n; memcpy(tb_c978rw, tb_c978_4, tb_c978_4n); tb_c978rw += tb_c978_4n; memcpy(tb_c978rw, tb_c978_5, tb_c978_5n); tb_c978rw += tb_c978_5n; memcpy(tb_c978rw, tb_c978_6, tb_c978_6n); tb_c978rw += tb_c978_6n; memcpy(tb_c978rw, tb_c978_7, tb_c978_7n); tb_c978rw += tb_c978_7n; *tb_c978rw = 0; (const char *)tb_c978r; }));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, ({ const char *tb_c979_0 = v_p; long long tb_c979_0n = (long long)strlen(tb_c979_0); const char *tb_c979_1 = ".len = "; long long tb_c979_1n = sizeof(".len = ") - 1; const char *tb_c979_2 = v_l; long long tb_c979_2n = (long long)strlen(tb_c979_2); const char *tb_c979_3 = "->len - "; long long tb_c979_3n = sizeof("->len - ") - 1; const char *tb_c979_4 = v_i; long long tb_c979_4n = (long long)strlen(tb_c979_4); const char *tb_c979_5 = " < "; long long tb_c979_5n = sizeof(" < ") - 1; const char *tb_c979_6 = v_m; long long tb_c979_6n = (long long)strlen(tb_c979_6); const char *tb_c979_7 = " ? "; long long tb_c979_7n = sizeof(" ? ") - 1; const char *tb_c979_8 = v_l; long long tb_c979_8n = (long long)strlen(tb_c979_8); const char *tb_c979_9 = "->len - "; long long tb_c979_9n = sizeof("->len - ") - 1; const char *tb_c979_10 = v_i; long long tb_c979_10n = (long long)strlen(tb_c979_10); const char *tb_c979_11 = " : "; long long tb_c979_11n = sizeof(" : ") - 1; const char *tb_c979_12 = v_m; long long tb_c979_12n = (long long)strlen(tb_c979_12); const char *tb_c979_13 = ";\n"; long long tb_c979_13n = sizeof(";\n") - 1; char *tb_c979r = tb_mem(tb_c979_0n + tb_c979_1n + tb_c979_2n + tb_c979_3n + tb_c979_4n + tb_c979_5n + tb_c979_6n + tb_c979_7n + tb_c979_8n + tb_c979_9n + tb_c979_10n + tb_c979_11n + tb_c979_12n + tb_c979_13n + 1), *tb_c979rw = tb_c979r; memcpy(tb_c979rw, tb_c979_0, tb_c979_0n); tb_c979rw += tb_c979_0n; memcpy(tb_c979rw, tb_c979_1, tb_c979_1n); tb_c979rw += tb_c979_1n; memcpy(tb_c979rw, tb_c979_2, tb_c979_2n); tb_c979rw += tb_c979_2n; memcpy(tb_c979rw, tb_c979_3, tb_c979_3n); tb_c979rw += tb_c979_3n; memcpy(tb_c979rw, tb_c979_4, tb_c979_4n); tb_c979rw += tb_c979_4n; memcpy(tb_c979rw, tb_c979_5, tb_c979_5n); tb_c979rw += tb_c979_5n; memcpy(tb_c979rw, tb_c979_6, tb_c979_6n); tb_c979rw += tb_c979_6n; memcpy(tb_c979rw, tb_c979_7, tb_c979_7n); tb_c979rw += tb_c979_7n; memcpy(tb_c979rw, tb_c979_8, tb_c979_8n); tb_c979rw += tb_c979_8n; memcpy(tb_c979rw, tb_c979_9, tb_c979_9n); tb_c979rw += tb_c979_9n; memcpy(tb_c979rw, tb_c979_10, tb_c979_10n); tb_c979rw += tb_c979_10n; memcpy(tb_c979rw, tb_c979_11, tb_c979_11n); tb_c979rw += tb_c979_11n; memcpy(tb_c979rw, tb_c979_12, tb_c979_12n); tb_c979rw += tb_c979_12n; memcpy(tb_c979rw, tb_c979_13, tb_c979_13n); tb_c979rw += tb_c979_13n; *tb_c979rw = 0; (const char *)tb_c979r; }));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, ({ const char *tb_c980_0 = "if ("; long long tb_c980_0n = sizeof("if (") - 1; const char *tb_c980_1 = v_i; long long tb_c980_1n = (long long)strlen(tb_c980_1); const char *tb_c980_2 = " + "; long long tb_c980_2n = sizeof(" + ") - 1; const char *tb_c980_3 = v_m; long long tb_c980_3n = (long long)strlen(tb_c980_3); const char *tb_c980_4 = " < "; long long tb_c980_4n = sizeof(" < ") - 1; const char *tb_c980_5 = v_l; long long tb_c980_5n = (long long)strlen(tb_c980_5); const char *tb_c980_6 = "->len) __builtin_prefetch("; long long tb_c980_6n = sizeof("->len) __builtin_prefetch(") - 1; const char *tb_c980_7 = v_p; long long tb_c980_7n = (long long)strlen(tb_c980_7); const char *tb_c980_8 = ".data + "; long long tb_c980_8n = sizeof(".data + ") - 1; const char *tb_c980_9 = v_m; long long tb_c980_9n = (long long)strlen(tb_c980_9); const char *tb_c980_10 = " * "; long long tb_c980_10n = sizeof(" * ") - 1; const char *tb_c980_11 = v_l; long long tb_c980_11n = (long long)strlen(tb_c980_11); const char *tb_c980_12 = "->size);\n"; long long tb_c980_12n = sizeof("->size);\n") - 1; char *tb_c980r = tb_mem(tb_c980_0n + tb_c980_1n + tb_c980_2n + tb_c980_3n + tb_c980_4n + tb_c980_5n + tb_c980_6n + tb_c980_7n + tb_c980_8n + tb_c980_9n + tb_c980_10n + tb_c980_11n + tb_c980_12n + 1), *tb_c980rw = tb_c980r; memcpy(tb_c980rw, tb_c980_0, tb_c980_0n); tb_c980rw += tb_c980_0n; memcpy(tb_c980rw, tb_c980_1, tb_c980_1n); tb_c980rw += tb_c980_1n; memcpy(tb_c980rw, tb_c980_2, tb_c980_2n); tb_c980rw += tb_c980_2n; memcpy(tb_c980rw, tb_c980_3, tb_c980_3n); tb_c980rw += tb_c980_3n; memcpy(tb_c980rw, tb_c980_4, tb_c980_4n); tb_c980rw += tb_c980_4n; memcpy(tb_c980rw, tb_c980_5, tb_c980_5n); tb_c980rw += tb_c980_5n; memcpy(tb_c980rw, tb_c980_6, tb_c980_6n); tb_c980rw += tb_c980_6n; memcpy(tb_c980rw, tb_c980_7, tb_c980_7n); tb_c980rw += tb_c980_7n; memcpy(tb_c980rw, tb_c980_8, tb_c980_8n); tb_c980rw += tb_c980_8n; memcpy(tb_c980rw, tb_c980_9, tb_c980_9n); tb_c980rw += tb_c980_9n; memcpy(tb_c980rw, tb_c980_10, tb_c980_10n); tb_c980rw += tb_c980_10n; memcpy(tb_c980rw, tb_c980_11, tb_c980_11n); tb_c980rw += tb_c980_11n; memcpy(tb_c980rw, tb_c980_12, tb_c980_12n); tb_c980rw += tb_c980_12n; *tb_c980rw = 0; (const char *)tb_c980r; }));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, ({ const char *tb_c981_0 = "tb_list * const v_"; long long tb_c981_0n = sizeof("tb_list * const v_") - 1; const char *tb_c981_1 = ((tb_o_Node *)tb_nn(v_n, 1056))->q_name; long long tb_c981_1n = (long long)strlen(tb_c981_1); const char *tb_c981_2 = " = &"; long long tb_c981_2n = sizeof(" = &") - 1; const char *tb_c981_3 = v_p; long long tb_c981_3n = (long long)strlen(tb_c981_3); const char *tb_c981_4 = ";\n"; long long tb_c981_4n = sizeof(";\n") - 1; char *tb_c981r = tb_mem(tb_c981_0n + tb_c981_1n + tb_c981_2n + tb_c981_3n + tb_c981_4n + 1), *tb_c981rw = tb_c981r; memcpy(tb_c981rw, tb_c981_0, tb_c981_0n); tb_c981rw += tb_c981_0n; memcpy(tb_c981rw, tb_c981_1, tb_c981_1n); tb_c981rw += tb_c981_1n; memcpy(tb_c981rw, tb_c981_2, tb_c981_2n); tb_c981rw += tb_c981_2n; memcpy(tb_c981rw, tb_c981_3, tb_c981_3n); tb_c981rw += tb_c981_3n; memcpy(tb_c981rw, tb_c981_4, tb_c981_4n); tb_c981rw += tb_c981_4n; *tb_c981rw = 0; (const char *)tb_c981r; }));
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1057))->q_kids, 1LL, sizeof(tb_o_Node *), 1057)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c982_0 = v_l; long long tb_c982_0n = (long long)strlen(tb_c982_0); const char *tb_c982_1 = "->cap = tb_cap"; long long tb_c982_1n = sizeof("->cap = tb_cap") - 1; const char *tb_c982_2 = v_id; long long tb_c982_2n = (long long)strlen(tb_c982_2); const char *tb_c982_3 = ";\n"; long long tb_c982_3n = sizeof(";\n") - 1; char *tb_c982r = tb_mem(tb_c982_0n + tb_c982_1n + tb_c982_2n + tb_c982_3n + 1), *tb_c982rw = tb_c982r; memcpy(tb_c982rw, tb_c982_0, tb_c982_0n); tb_c982rw += tb_c982_0n; memcpy(tb_c982rw, tb_c982_1, tb_c982_1n); tb_c982rw += tb_c982_1n; memcpy(tb_c982rw, tb_c982_2, tb_c982_2n); tb_c982rw += tb_c982_2n; memcpy(tb_c982rw, tb_c982_3, tb_c982_3n); tb_c982rw += tb_c982_3n; *tb_c982rw = 0; (const char *)tb_c982r; }));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_background(tb_o_Comp * v_c, tb_o_Node * v_n) {
    { long long tb_v983 = 1LL; long long *tb_p983 = &(((tb_o_Comp *)tb_nn(v_c, 1069))->q_label); *tb_p983 += tb_v983; }
    const char * v_id = ({ const char *tb_c984_0 = ""; long long tb_c984_0n = sizeof("") - 1; char tb_c984_1d[24]; long long tb_c984_1n; const char *tb_c984_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1070))->q_label, tb_c984_1d, &tb_c984_1n); char *tb_c984r = tb_mem(tb_c984_0n + tb_c984_1n + 1), *tb_c984rw = tb_c984r; memcpy(tb_c984rw, tb_c984_0, tb_c984_0n); tb_c984rw += tb_c984_0n; memcpy(tb_c984rw, tb_c984_1, tb_c984_1n); tb_c984rw += tb_c984_1n; *tb_c984rw = 0; (const char *)tb_c984r; });
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1071))->q_kids, 0LL, sizeof(tb_o_Node *), 1071));
    tb_o_Node * v_f = ((tb_o_Node *)tb_nn(v_call, 1072))->q_ref;
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1073))->q_kids, 0LL, sizeof(tb_o_Node *), 1073));
    long long v_rt = ((tb_o_Node *)tb_nn(v_f, 1074))->q_ty;
    if ((v_rt < 0LL)) {
        v_rt = 0LL;
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 1077))->q_out;
    tb_list * v_w = tb_list_new(sizeof(const char *));
    { tb_list * tb_v985 = v_w; tb_list * *tb_p985 = &(((tb_o_Comp *)tb_nn(v_c, 1079))->q_out); *tb_p985 = tb_v985; }
    f_emit__put(v_c, "\ntypedef struct {\n    tb_task tb_t;\n");
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, ({ const char *tb_c986_0 = "    "; long long tb_c986_0n = sizeof("    ") - 1; const char *tb_c986_1 = f_emit__c_type(v_c, v_rt); long long tb_c986_1n = (long long)strlen(tb_c986_1); const char *tb_c986_2 = " tb_r;\n"; long long tb_c986_2n = sizeof(" tb_r;\n") - 1; char *tb_c986r = tb_mem(tb_c986_0n + tb_c986_1n + tb_c986_2n + 1), *tb_c986rw = tb_c986r; memcpy(tb_c986rw, tb_c986_0, tb_c986_0n); tb_c986rw += tb_c986_0n; memcpy(tb_c986rw, tb_c986_1, tb_c986_1n); tb_c986rw += tb_c986_1n; memcpy(tb_c986rw, tb_c986_2, tb_c986_2n); tb_c986rw += tb_c986_2n; *tb_c986rw = 0; (const char *)tb_c986r; }));
    }
    {
        long long tb_end987 = ((((tb_o_Node *)tb_nn(v_params, 1083))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end987; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c988_0 = "    "; long long tb_c988_0n = sizeof("    ") - 1; const char *tb_c988_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1084))->q_kids, v_i, sizeof(tb_o_Node *), 1084)), 1084))->q_ty); long long tb_c988_1n = (long long)strlen(tb_c988_1); const char *tb_c988_2 = " a"; long long tb_c988_2n = sizeof(" a") - 1; char tb_c988_3d[24]; long long tb_c988_3n; const char *tb_c988_3 = tb_digits(v_i, tb_c988_3d, &tb_c988_3n); const char *tb_c988_4 = ";\n"; long long tb_c988_4n = sizeof(";\n") - 1; char *tb_c988r = tb_mem(tb_c988_0n + tb_c988_1n + tb_c988_2n + tb_c988_3n + tb_c988_4n + 1), *tb_c988rw = tb_c988r; memcpy(tb_c988rw, tb_c988_0, tb_c988_0n); tb_c988rw += tb_c988_0n; memcpy(tb_c988rw, tb_c988_1, tb_c988_1n); tb_c988rw += tb_c988_1n; memcpy(tb_c988rw, tb_c988_2, tb_c988_2n); tb_c988rw += tb_c988_2n; memcpy(tb_c988rw, tb_c988_3, tb_c988_3n); tb_c988rw += tb_c988_3n; memcpy(tb_c988rw, tb_c988_4, tb_c988_4n); tb_c988rw += tb_c988_4n; *tb_c988rw = 0; (const char *)tb_c988r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c989_0 = "} tb_bgjob"; long long tb_c989_0n = sizeof("} tb_bgjob") - 1; const char *tb_c989_1 = v_id; long long tb_c989_1n = (long long)strlen(tb_c989_1); const char *tb_c989_2 = ";\n"; long long tb_c989_2n = sizeof(";\n") - 1; char *tb_c989r = tb_mem(tb_c989_0n + tb_c989_1n + tb_c989_2n + 1), *tb_c989rw = tb_c989r; memcpy(tb_c989rw, tb_c989_0, tb_c989_0n); tb_c989rw += tb_c989_0n; memcpy(tb_c989rw, tb_c989_1, tb_c989_1n); tb_c989rw += tb_c989_1n; memcpy(tb_c989rw, tb_c989_2, tb_c989_2n); tb_c989rw += tb_c989_2n; *tb_c989rw = 0; (const char *)tb_c989r; }));
    f_emit__put(v_c, ({ const char *tb_c990_0 = "static void *tb_bgrun"; long long tb_c990_0n = sizeof("static void *tb_bgrun") - 1; const char *tb_c990_1 = v_id; long long tb_c990_1n = (long long)strlen(tb_c990_1); const char *tb_c990_2 = "(void *tb_arg) {\n    tb_bgjob"; long long tb_c990_2n = sizeof("(void *tb_arg) {\n    tb_bgjob") - 1; const char *tb_c990_3 = v_id; long long tb_c990_3n = (long long)strlen(tb_c990_3); const char *tb_c990_4 = " *tb_j = tb_arg;\n    "; long long tb_c990_4n = sizeof(" *tb_j = tb_arg;\n    ") - 1; char *tb_c990r = tb_mem(tb_c990_0n + tb_c990_1n + tb_c990_2n + tb_c990_3n + tb_c990_4n + 1), *tb_c990rw = tb_c990r; memcpy(tb_c990rw, tb_c990_0, tb_c990_0n); tb_c990rw += tb_c990_0n; memcpy(tb_c990rw, tb_c990_1, tb_c990_1n); tb_c990rw += tb_c990_1n; memcpy(tb_c990rw, tb_c990_2, tb_c990_2n); tb_c990rw += tb_c990_2n; memcpy(tb_c990rw, tb_c990_3, tb_c990_3n); tb_c990rw += tb_c990_3n; memcpy(tb_c990rw, tb_c990_4, tb_c990_4n); tb_c990rw += tb_c990_4n; *tb_c990rw = 0; (const char *)tb_c990r; }));
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, "tb_j->tb_r = ");
    }
    f_emit__put(v_c, ({ const char *tb_c991_0 = f_emit__fname(v_f); long long tb_c991_0n = (long long)strlen(tb_c991_0); const char *tb_c991_1 = "("; long long tb_c991_1n = sizeof("(") - 1; char *tb_c991r = tb_mem(tb_c991_0n + tb_c991_1n + 1), *tb_c991rw = tb_c991r; memcpy(tb_c991rw, tb_c991_0, tb_c991_0n); tb_c991rw += tb_c991_0n; memcpy(tb_c991rw, tb_c991_1, tb_c991_1n); tb_c991rw += tb_c991_1n; *tb_c991rw = 0; (const char *)tb_c991r; }));
    {
        long long tb_end992 = ((((tb_o_Node *)tb_nn(v_params, 1090))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end992; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c993_0 = "tb_j->a"; long long tb_c993_0n = sizeof("tb_j->a") - 1; char tb_c993_1d[24]; long long tb_c993_1n; const char *tb_c993_1 = tb_digits(v_i, tb_c993_1d, &tb_c993_1n); char *tb_c993r = tb_mem(tb_c993_0n + tb_c993_1n + 1), *tb_c993rw = tb_c993r; memcpy(tb_c993rw, tb_c993_0, tb_c993_0n); tb_c993rw += tb_c993_0n; memcpy(tb_c993rw, tb_c993_1, tb_c993_1n); tb_c993rw += tb_c993_1n; *tb_c993rw = 0; (const char *)tb_c993r; }));
        }
    }
    f_emit__put(v_c, ");\n    return 0;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c994_0 = "static tb_task *tb_bg"; long long tb_c994_0n = sizeof("static tb_task *tb_bg") - 1; const char *tb_c994_1 = v_id; long long tb_c994_1n = (long long)strlen(tb_c994_1); const char *tb_c994_2 = "("; long long tb_c994_2n = sizeof("(") - 1; char *tb_c994r = tb_mem(tb_c994_0n + tb_c994_1n + tb_c994_2n + 1), *tb_c994rw = tb_c994r; memcpy(tb_c994rw, tb_c994_0, tb_c994_0n); tb_c994rw += tb_c994_0n; memcpy(tb_c994rw, tb_c994_1, tb_c994_1n); tb_c994rw += tb_c994_1n; memcpy(tb_c994rw, tb_c994_2, tb_c994_2n); tb_c994rw += tb_c994_2n; *tb_c994rw = 0; (const char *)tb_c994r; }));
    {
        long long tb_end995 = ((((tb_o_Node *)tb_nn(v_params, 1096))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end995; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c996_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1099))->q_kids, v_i, sizeof(tb_o_Node *), 1099)), 1099))->q_ty); long long tb_c996_0n = (long long)strlen(tb_c996_0); const char *tb_c996_1 = " a"; long long tb_c996_1n = sizeof(" a") - 1; char tb_c996_2d[24]; long long tb_c996_2n; const char *tb_c996_2 = tb_digits(v_i, tb_c996_2d, &tb_c996_2n); char *tb_c996r = tb_mem(tb_c996_0n + tb_c996_1n + tb_c996_2n + 1), *tb_c996rw = tb_c996r; memcpy(tb_c996rw, tb_c996_0, tb_c996_0n); tb_c996rw += tb_c996_0n; memcpy(tb_c996rw, tb_c996_1, tb_c996_1n); tb_c996rw += tb_c996_1n; memcpy(tb_c996rw, tb_c996_2, tb_c996_2n); tb_c996rw += tb_c996_2n; *tb_c996rw = 0; (const char *)tb_c996r; }));
        }
    }
    if (((((tb_o_Node *)tb_nn(v_params, 1100))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    f_emit__put(v_c, ({ const char *tb_c997_0 = ") {\n    tb_bgjob"; long long tb_c997_0n = sizeof(") {\n    tb_bgjob") - 1; const char *tb_c997_1 = v_id; long long tb_c997_1n = (long long)strlen(tb_c997_1); const char *tb_c997_2 = " *tb_j = malloc(sizeof(tb_bgjob"; long long tb_c997_2n = sizeof(" *tb_j = malloc(sizeof(tb_bgjob") - 1; const char *tb_c997_3 = v_id; long long tb_c997_3n = (long long)strlen(tb_c997_3); const char *tb_c997_4 = "));\n"; long long tb_c997_4n = sizeof("));\n") - 1; char *tb_c997r = tb_mem(tb_c997_0n + tb_c997_1n + tb_c997_2n + tb_c997_3n + tb_c997_4n + 1), *tb_c997rw = tb_c997r; memcpy(tb_c997rw, tb_c997_0, tb_c997_0n); tb_c997rw += tb_c997_0n; memcpy(tb_c997rw, tb_c997_1, tb_c997_1n); tb_c997rw += tb_c997_1n; memcpy(tb_c997rw, tb_c997_2, tb_c997_2n); tb_c997rw += tb_c997_2n; memcpy(tb_c997rw, tb_c997_3, tb_c997_3n); tb_c997rw += tb_c997_3n; memcpy(tb_c997rw, tb_c997_4, tb_c997_4n); tb_c997rw += tb_c997_4n; *tb_c997rw = 0; (const char *)tb_c997r; }));
    f_emit__put(v_c, "    if (tb_j == 0) tb_crash(0, \"не хватило памяти\");\n");
    {
        long long tb_end998 = ((((tb_o_Node *)tb_nn(v_params, 1104))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end998; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c999_0 = "    tb_j->a"; long long tb_c999_0n = sizeof("    tb_j->a") - 1; char tb_c999_1d[24]; long long tb_c999_1n; const char *tb_c999_1 = tb_digits(v_i, tb_c999_1d, &tb_c999_1n); const char *tb_c999_2 = " = a"; long long tb_c999_2n = sizeof(" = a") - 1; char tb_c999_3d[24]; long long tb_c999_3n; const char *tb_c999_3 = tb_digits(v_i, tb_c999_3d, &tb_c999_3n); const char *tb_c999_4 = ";\n"; long long tb_c999_4n = sizeof(";\n") - 1; char *tb_c999r = tb_mem(tb_c999_0n + tb_c999_1n + tb_c999_2n + tb_c999_3n + tb_c999_4n + 1), *tb_c999rw = tb_c999r; memcpy(tb_c999rw, tb_c999_0, tb_c999_0n); tb_c999rw += tb_c999_0n; memcpy(tb_c999rw, tb_c999_1, tb_c999_1n); tb_c999rw += tb_c999_1n; memcpy(tb_c999rw, tb_c999_2, tb_c999_2n); tb_c999rw += tb_c999_2n; memcpy(tb_c999rw, tb_c999_3, tb_c999_3n); tb_c999rw += tb_c999_3n; memcpy(tb_c999rw, tb_c999_4, tb_c999_4n); tb_c999rw += tb_c999_4n; *tb_c999rw = 0; (const char *)tb_c999r; }));
        }
    }
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, "    tb_j->tb_t.res = &tb_j->tb_r;\n");
    } else {
        f_emit__put(v_c, "    tb_j->tb_t.res = 0;\n");
    }
    f_emit__put(v_c, ({ const char *tb_c1000_0 = "    tb_task_start(&tb_j->tb_t, tb_bgrun"; long long tb_c1000_0n = sizeof("    tb_task_start(&tb_j->tb_t, tb_bgrun") - 1; const char *tb_c1000_1 = v_id; long long tb_c1000_1n = (long long)strlen(tb_c1000_1); const char *tb_c1000_2 = ", tb_j);\n    return &tb_j->tb_t;\n}\n"; long long tb_c1000_2n = sizeof(", tb_j);\n    return &tb_j->tb_t;\n}\n") - 1; char *tb_c1000r = tb_mem(tb_c1000_0n + tb_c1000_1n + tb_c1000_2n + 1), *tb_c1000rw = tb_c1000r; memcpy(tb_c1000rw, tb_c1000_0, tb_c1000_0n); tb_c1000rw += tb_c1000_0n; memcpy(tb_c1000rw, tb_c1000_1, tb_c1000_1n); tb_c1000rw += tb_c1000_1n; memcpy(tb_c1000rw, tb_c1000_2, tb_c1000_2n); tb_c1000rw += tb_c1000_2n; *tb_c1000rw = 0; (const char *)tb_c1000r; }));
    { tb_list * tb_v1001 = v_saved; tb_list * *tb_p1001 = &(((tb_o_Comp *)tb_nn(v_c, 1111))->q_out); *tb_p1001 = tb_v1001; }
    { const char * tb_v1002 = tb_join(v_w, ""); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1112))->q_workers) = tb_v1002; }
    f_emit__put(v_c, ({ const char *tb_c1003_0 = "tb_bg"; long long tb_c1003_0n = sizeof("tb_bg") - 1; const char *tb_c1003_1 = v_id; long long tb_c1003_1n = (long long)strlen(tb_c1003_1); const char *tb_c1003_2 = "("; long long tb_c1003_2n = sizeof("(") - 1; char *tb_c1003r = tb_mem(tb_c1003_0n + tb_c1003_1n + tb_c1003_2n + 1), *tb_c1003rw = tb_c1003r; memcpy(tb_c1003rw, tb_c1003_0, tb_c1003_0n); tb_c1003rw += tb_c1003_0n; memcpy(tb_c1003rw, tb_c1003_1, tb_c1003_1n); tb_c1003rw += tb_c1003_1n; memcpy(tb_c1003rw, tb_c1003_2, tb_c1003_2n); tb_c1003rw += tb_c1003_2n; *tb_c1003rw = 0; (const char *)tb_c1003r; }));
    {
        long long tb_end1004 = ((((tb_o_Node *)tb_nn(v_call, 1114))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1004; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_call, 1117))->q_kids, v_i, sizeof(tb_o_Node *), 1117)), 1117))->q_kids, 0LL, sizeof(tb_o_Node *), 1117)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1117))->q_kids, v_i, sizeof(tb_o_Node *), 1117)), 1117))->q_ty);
        }
    }
    f_emit__put(v_c, ")");
}

static _Bool f_emit__is_pure(tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1122))->q_kind;
    if (((((({ const char *tb_s1005 = v_k; ((unsigned char)tb_s1005[0] == 110 && (unsigned char)tb_s1005[1] == 117 && (unsigned char)tb_s1005[2] == 109 && (unsigned char)tb_s1005[3] == 98 && (unsigned char)tb_s1005[4] == 101 && (unsigned char)tb_s1005[5] == 114 && tb_s1005[6] == 0); }) || ({ const char *tb_s1006 = v_k; ((unsigned char)tb_s1006[0] == 100 && (unsigned char)tb_s1006[1] == 101 && (unsigned char)tb_s1006[2] == 99 && (unsigned char)tb_s1006[3] == 105 && (unsigned char)tb_s1006[4] == 109 && (unsigned char)tb_s1006[5] == 97 && (unsigned char)tb_s1006[6] == 108 && tb_s1006[7] == 0); })) || ({ const char *tb_s1007 = v_k; ((unsigned char)tb_s1007[0] == 115 && (unsigned char)tb_s1007[1] == 116 && (unsigned char)tb_s1007[2] == 114 && (unsigned char)tb_s1007[3] == 105 && (unsigned char)tb_s1007[4] == 110 && (unsigned char)tb_s1007[5] == 103 && tb_s1007[6] == 0); })) || ({ const char *tb_s1008 = v_k; ((unsigned char)tb_s1008[0] == 98 && (unsigned char)tb_s1008[1] == 111 && (unsigned char)tb_s1008[2] == 111 && (unsigned char)tb_s1008[3] == 108 && tb_s1008[4] == 0); })) || ({ const char *tb_s1009 = v_k; ((unsigned char)tb_s1009[0] == 110 && (unsigned char)tb_s1009[1] == 97 && (unsigned char)tb_s1009[2] == 109 && (unsigned char)tb_s1009[3] == 101 && tb_s1009[4] == 0); }))) {
        return 1;
    }
    if ((({ const char *tb_s1010 = v_k; ((unsigned char)tb_s1010[0] == 102 && (unsigned char)tb_s1010[1] == 105 && (unsigned char)tb_s1010[2] == 101 && (unsigned char)tb_s1010[3] == 108 && (unsigned char)tb_s1010[4] == 100 && tb_s1010[5] == 0); }) || ({ const char *tb_s1011 = v_k; ((unsigned char)tb_s1011[0] == 117 && (unsigned char)tb_s1011[1] == 110 && (unsigned char)tb_s1011[2] == 97 && (unsigned char)tb_s1011[3] == 114 && (unsigned char)tb_s1011[4] == 121 && tb_s1011[5] == 0); }))) {
        return f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1126))->q_kids, 0LL, sizeof(tb_o_Node *), 1126)));
    }
    if (({ const char *tb_s1012 = v_k; ((unsigned char)tb_s1012[0] == 98 && (unsigned char)tb_s1012[1] == 105 && (unsigned char)tb_s1012[2] == 110 && (unsigned char)tb_s1012[3] == 111 && (unsigned char)tb_s1012[4] == 112 && tb_s1012[5] == 0); })) {
        if ((({ const char *tb_s1013 = ((tb_o_Node *)tb_nn(v_n, 1128))->q_value; ((unsigned char)tb_s1013[0] == 47 && tb_s1013[1] == 0); }) || ({ const char *tb_s1014 = ((tb_o_Node *)tb_nn(v_n, 1128))->q_value; ((unsigned char)tb_s1014[0] == 37 && tb_s1014[1] == 0); }))) {
            return 0;
        }
        return (f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1130))->q_kids, 0LL, sizeof(tb_o_Node *), 1130))) && f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1130))->q_kids, 1LL, sizeof(tb_o_Node *), 1130))));
    }
    return 0;
    tb_crash(1121, "функция is_pure дошла до конца без return");
    return 0;
}

static void f_emit__emit_together(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v1015 = 1LL; long long *tb_p1015 = &(((tb_o_Comp *)tb_nn(v_c, 1139))->q_label); *tb_p1015 += tb_v1015; }
    const char * v_id = ({ const char *tb_c1016_0 = ""; long long tb_c1016_0n = sizeof("") - 1; char tb_c1016_1d[24]; long long tb_c1016_1n; const char *tb_c1016_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1140))->q_label, tb_c1016_1d, &tb_c1016_1n); char *tb_c1016r = tb_mem(tb_c1016_0n + tb_c1016_1n + 1), *tb_c1016rw = tb_c1016r; memcpy(tb_c1016rw, tb_c1016_0, tb_c1016_0n); tb_c1016rw += tb_c1016_0n; memcpy(tb_c1016rw, tb_c1016_1, tb_c1016_1n); tb_c1016rw += tb_c1016_1n; *tb_c1016rw = 0; (const char *)tb_c1016r; });
    tb_o_Node * v_block = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1141))->q_kids, 0LL, sizeof(tb_o_Node *), 1141));
    {
        long long tb_end1017 = ((((tb_o_Node *)tb_nn(v_block, 1142))->q_kids)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end1017; v_j++) {
            tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_block, 1143))->q_kids, v_j, sizeof(tb_o_Node *), 1143)), 1143))->q_kids, 0LL, sizeof(tb_o_Node *), 1143));
            if (((({ const char *tb_s1018 = ((tb_o_Node *)tb_nn(v_v, 1144))->q_kind; !((unsigned char)tb_s1018[0] == 105 && (unsigned char)tb_s1018[1] == 110 && (unsigned char)tb_s1018[2] == 100 && (unsigned char)tb_s1018[3] == 101 && (unsigned char)tb_s1018[4] == 120 && tb_s1018[5] == 0); }) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1144))->q_kids, 0LL, sizeof(tb_o_Node *), 1144))))) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1144))->q_kids, 1LL, sizeof(tb_o_Node *), 1144)))))) {
                continue;
            }
            f_emit__indent(v_c, v_depth);
            if (({ const char *tb_s1019 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1147))->q_kids, 0LL, sizeof(tb_o_Node *), 1147)), 1147))->q_ty); ((unsigned char)tb_s1019[0] == 100 && (unsigned char)tb_s1019[1] == 105 && (unsigned char)tb_s1019[2] == 99 && (unsigned char)tb_s1019[3] == 116 && tb_s1019[4] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c1020_0 = "unsigned long long tb_h"; long long tb_c1020_0n = sizeof("unsigned long long tb_h") - 1; const char *tb_c1020_1 = v_id; long long tb_c1020_1n = (long long)strlen(tb_c1020_1); const char *tb_c1020_2 = "_"; long long tb_c1020_2n = sizeof("_") - 1; char tb_c1020_3d[24]; long long tb_c1020_3n; const char *tb_c1020_3 = tb_digits(v_j, tb_c1020_3d, &tb_c1020_3n); const char *tb_c1020_4 = " = tb_dpre1("; long long tb_c1020_4n = sizeof(" = tb_dpre1(") - 1; char *tb_c1020r = tb_mem(tb_c1020_0n + tb_c1020_1n + tb_c1020_2n + tb_c1020_3n + tb_c1020_4n + 1), *tb_c1020rw = tb_c1020r; memcpy(tb_c1020rw, tb_c1020_0, tb_c1020_0n); tb_c1020rw += tb_c1020_0n; memcpy(tb_c1020rw, tb_c1020_1, tb_c1020_1n); tb_c1020rw += tb_c1020_1n; memcpy(tb_c1020rw, tb_c1020_2, tb_c1020_2n); tb_c1020rw += tb_c1020_2n; memcpy(tb_c1020rw, tb_c1020_3, tb_c1020_3n); tb_c1020rw += tb_c1020_3n; memcpy(tb_c1020rw, tb_c1020_4, tb_c1020_4n); tb_c1020rw += tb_c1020_4n; *tb_c1020rw = 0; (const char *)tb_c1020r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1149))->q_kids, 0LL, sizeof(tb_o_Node *), 1149)));
                f_emit__put(v_c, ", ");
                f_emit__emit_key(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1151))->q_kids, 0LL, sizeof(tb_o_Node *), 1151)), 1151))->q_ty, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1151))->q_kids, 1LL, sizeof(tb_o_Node *), 1151)));
                f_emit__put(v_c, ");\n");
            } else if ((f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1153))->q_kids, 0LL, sizeof(tb_o_Node *), 1153)), 1153))->q_ty) && (!f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1153))->q_kids, 0LL, sizeof(tb_o_Node *), 1153)), 1153))->q_ty)))) {
                f_emit__put(v_c, "tb_lpre(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1155))->q_kids, 0LL, sizeof(tb_o_Node *), 1155)));
                f_emit__put(v_c, ", ");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1157))->q_kids, 1LL, sizeof(tb_o_Node *), 1157)));
                f_emit__put(v_c, ");\n");
            } else {
                f_emit__put(v_c, ";\n");
            }
        }
    }
    {
        long long tb_end1021 = ((((tb_o_Node *)tb_nn(v_block, 1161))->q_kids)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end1021; v_j++) {
            tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_block, 1162))->q_kids, v_j, sizeof(tb_o_Node *), 1162)), 1162))->q_kids, 0LL, sizeof(tb_o_Node *), 1162));
            if (((({ const char *tb_s1022 = ((tb_o_Node *)tb_nn(v_v, 1163))->q_kind; !((unsigned char)tb_s1022[0] == 105 && (unsigned char)tb_s1022[1] == 110 && (unsigned char)tb_s1022[2] == 100 && (unsigned char)tb_s1022[3] == 101 && (unsigned char)tb_s1022[4] == 120 && tb_s1022[5] == 0); }) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1163))->q_kids, 0LL, sizeof(tb_o_Node *), 1163))))) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1163))->q_kids, 1LL, sizeof(tb_o_Node *), 1163)))))) {
                continue;
            }
            if (({ const char *tb_s1023 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1165))->q_kids, 0LL, sizeof(tb_o_Node *), 1165)), 1165))->q_ty); ((unsigned char)tb_s1023[0] == 100 && (unsigned char)tb_s1023[1] == 105 && (unsigned char)tb_s1023[2] == 99 && (unsigned char)tb_s1023[3] == 116 && tb_s1023[4] == 0); })) {
                f_emit__indent(v_c, v_depth);
                f_emit__put(v_c, "tb_dpre2(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1168))->q_kids, 0LL, sizeof(tb_o_Node *), 1168)));
                f_emit__put(v_c, ({ const char *tb_c1024_0 = ", tb_h"; long long tb_c1024_0n = sizeof(", tb_h") - 1; const char *tb_c1024_1 = v_id; long long tb_c1024_1n = (long long)strlen(tb_c1024_1); const char *tb_c1024_2 = "_"; long long tb_c1024_2n = sizeof("_") - 1; char tb_c1024_3d[24]; long long tb_c1024_3n; const char *tb_c1024_3 = tb_digits(v_j, tb_c1024_3d, &tb_c1024_3n); const char *tb_c1024_4 = ");\n"; long long tb_c1024_4n = sizeof(");\n") - 1; char *tb_c1024r = tb_mem(tb_c1024_0n + tb_c1024_1n + tb_c1024_2n + tb_c1024_3n + tb_c1024_4n + 1), *tb_c1024rw = tb_c1024r; memcpy(tb_c1024rw, tb_c1024_0, tb_c1024_0n); tb_c1024rw += tb_c1024_0n; memcpy(tb_c1024rw, tb_c1024_1, tb_c1024_1n); tb_c1024rw += tb_c1024_1n; memcpy(tb_c1024rw, tb_c1024_2, tb_c1024_2n); tb_c1024rw += tb_c1024_2n; memcpy(tb_c1024rw, tb_c1024_3, tb_c1024_3n); tb_c1024rw += tb_c1024_3n; memcpy(tb_c1024rw, tb_c1024_4, tb_c1024_4n); tb_c1024rw += tb_c1024_4n; *tb_c1024rw = 0; (const char *)tb_c1024r; }));
            }
        }
    }
    {
        tb_list * tb_l1025 = ((tb_o_Node *)tb_nn(v_block, 1170))->q_kids;
        for (long long tb_i1025 = 0; tb_i1025 < tb_l1025->len; tb_i1025++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l1025->data)[tb_i1025];
            f_emit__emit_statement(v_c, v_s, v_depth);
        }
    }
}

static void f_emit__emit_prefetch(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth, const char * v_list_c, const char * v_i_c, const char * v_elem) {
    {
        long long tb_end1026 = ((((tb_o_Node *)tb_nn(v_n, 1177))->q_pre_bases)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end1026; v_j++) {
            { long long tb_v1027 = 1LL; long long *tb_p1027 = &(((tb_o_Comp *)tb_nn(v_c, 1178))->q_label); *tb_p1027 += tb_v1027; }
            const char * v_f = ({ const char *tb_c1028_0 = "tb_f"; long long tb_c1028_0n = sizeof("tb_f") - 1; char tb_c1028_1d[24]; long long tb_c1028_1n; const char *tb_c1028_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1179))->q_label, tb_c1028_1d, &tb_c1028_1n); char *tb_c1028r = tb_mem(tb_c1028_0n + tb_c1028_1n + 1), *tb_c1028rw = tb_c1028r; memcpy(tb_c1028rw, tb_c1028_0, tb_c1028_0n); tb_c1028rw += tb_c1028_0n; memcpy(tb_c1028rw, tb_c1028_1, tb_c1028_1n); tb_c1028rw += tb_c1028_1n; *tb_c1028rw = 0; (const char *)tb_c1028r; });
            const char * v_plan = v_list_c;
            f_emit__indent(v_c, v_depth);
            if (({ const char *tb_s1029 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1182))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1182)), 1182))->q_kind; ((unsigned char)tb_s1029[0] == 110 && (unsigned char)tb_s1029[1] == 97 && (unsigned char)tb_s1029[2] == 109 && (unsigned char)tb_s1029[3] == 101 && tb_s1029[4] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c1030_0 = "{ tb_list *tb_q"; long long tb_c1030_0n = sizeof("{ tb_list *tb_q") - 1; char tb_c1030_1d[24]; long long tb_c1030_1n; const char *tb_c1030_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1183))->q_label, tb_c1030_1d, &tb_c1030_1n); const char *tb_c1030_2 = " = "; long long tb_c1030_2n = sizeof(" = ") - 1; char *tb_c1030r = tb_mem(tb_c1030_0n + tb_c1030_1n + tb_c1030_2n + 1), *tb_c1030rw = tb_c1030r; memcpy(tb_c1030rw, tb_c1030_0, tb_c1030_0n); tb_c1030rw += tb_c1030_0n; memcpy(tb_c1030rw, tb_c1030_1, tb_c1030_1n); tb_c1030rw += tb_c1030_1n; memcpy(tb_c1030rw, tb_c1030_2, tb_c1030_2n); tb_c1030rw += tb_c1030_2n; *tb_c1030rw = 0; (const char *)tb_c1030r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1184))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1184)));
                f_emit__put(v_c, "; ");
                v_plan = ({ const char *tb_c1031_0 = "tb_q"; long long tb_c1031_0n = sizeof("tb_q") - 1; char tb_c1031_1d[24]; long long tb_c1031_1n; const char *tb_c1031_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1186))->q_label, tb_c1031_1d, &tb_c1031_1n); char *tb_c1031r = tb_mem(tb_c1031_0n + tb_c1031_1n + 1), *tb_c1031rw = tb_c1031r; memcpy(tb_c1031rw, tb_c1031_0, tb_c1031_0n); tb_c1031rw += tb_c1031_0n; memcpy(tb_c1031rw, tb_c1031_1, tb_c1031_1n); tb_c1031rw += tb_c1031_1n; *tb_c1031rw = 0; (const char *)tb_c1031r; });
            } else {
                f_emit__put(v_c, "{ ");
            }
            f_emit__put(v_c, ({ const char *tb_c1032_0 = "if ((unsigned long long)("; long long tb_c1032_0n = sizeof("if ((unsigned long long)(") - 1; const char *tb_c1032_1 = v_i_c; long long tb_c1032_1n = (long long)strlen(tb_c1032_1); const char *tb_c1032_2 = " + 16) < (unsigned long long)"; long long tb_c1032_2n = sizeof(" + 16) < (unsigned long long)") - 1; const char *tb_c1032_3 = v_plan; long long tb_c1032_3n = (long long)strlen(tb_c1032_3); const char *tb_c1032_4 = "->len) { "; long long tb_c1032_4n = sizeof("->len) { ") - 1; char *tb_c1032r = tb_mem(tb_c1032_0n + tb_c1032_1n + tb_c1032_2n + tb_c1032_3n + tb_c1032_4n + 1), *tb_c1032rw = tb_c1032r; memcpy(tb_c1032rw, tb_c1032_0, tb_c1032_0n); tb_c1032rw += tb_c1032_0n; memcpy(tb_c1032rw, tb_c1032_1, tb_c1032_1n); tb_c1032rw += tb_c1032_1n; memcpy(tb_c1032rw, tb_c1032_2, tb_c1032_2n); tb_c1032rw += tb_c1032_2n; memcpy(tb_c1032rw, tb_c1032_3, tb_c1032_3n); tb_c1032rw += tb_c1032_3n; memcpy(tb_c1032rw, tb_c1032_4, tb_c1032_4n); tb_c1032rw += tb_c1032_4n; *tb_c1032rw = 0; (const char *)tb_c1032r; }));
            const char * v_el = v_elem;
            if (({ const char *tb_s1033 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1191))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1191)), 1191))->q_kind; ((unsigned char)tb_s1033[0] == 110 && (unsigned char)tb_s1033[1] == 97 && (unsigned char)tb_s1033[2] == 109 && (unsigned char)tb_s1033[3] == 101 && tb_s1033[4] == 0); })) {
                v_el = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1192))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1192))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1192)), 1192))->q_ty, sizeof(tb_o_Type *), 1192)), 1192))->q_elem);
            }
            f_emit__put(v_c, ({ const char *tb_c1034_0 = v_el; long long tb_c1034_0n = (long long)strlen(tb_c1034_0); const char *tb_c1034_1 = " "; long long tb_c1034_1n = sizeof(" ") - 1; const char *tb_c1034_2 = v_f; long long tb_c1034_2n = (long long)strlen(tb_c1034_2); const char *tb_c1034_3 = " = (("; long long tb_c1034_3n = sizeof(" = ((") - 1; const char *tb_c1034_4 = v_el; long long tb_c1034_4n = (long long)strlen(tb_c1034_4); const char *tb_c1034_5 = " *)"; long long tb_c1034_5n = sizeof(" *)") - 1; const char *tb_c1034_6 = v_plan; long long tb_c1034_6n = (long long)strlen(tb_c1034_6); const char *tb_c1034_7 = "->data)["; long long tb_c1034_7n = sizeof("->data)[") - 1; const char *tb_c1034_8 = v_i_c; long long tb_c1034_8n = (long long)strlen(tb_c1034_8); const char *tb_c1034_9 = " + 16]; "; long long tb_c1034_9n = sizeof(" + 16]; ") - 1; char *tb_c1034r = tb_mem(tb_c1034_0n + tb_c1034_1n + tb_c1034_2n + tb_c1034_3n + tb_c1034_4n + tb_c1034_5n + tb_c1034_6n + tb_c1034_7n + tb_c1034_8n + tb_c1034_9n + 1), *tb_c1034rw = tb_c1034r; memcpy(tb_c1034rw, tb_c1034_0, tb_c1034_0n); tb_c1034rw += tb_c1034_0n; memcpy(tb_c1034rw, tb_c1034_1, tb_c1034_1n); tb_c1034rw += tb_c1034_1n; memcpy(tb_c1034rw, tb_c1034_2, tb_c1034_2n); tb_c1034rw += tb_c1034_2n; memcpy(tb_c1034rw, tb_c1034_3, tb_c1034_3n); tb_c1034rw += tb_c1034_3n; memcpy(tb_c1034rw, tb_c1034_4, tb_c1034_4n); tb_c1034rw += tb_c1034_4n; memcpy(tb_c1034rw, tb_c1034_5, tb_c1034_5n); tb_c1034rw += tb_c1034_5n; memcpy(tb_c1034rw, tb_c1034_6, tb_c1034_6n); tb_c1034rw += tb_c1034_6n; memcpy(tb_c1034rw, tb_c1034_7, tb_c1034_7n); tb_c1034rw += tb_c1034_7n; memcpy(tb_c1034rw, tb_c1034_8, tb_c1034_8n); tb_c1034rw += tb_c1034_8n; memcpy(tb_c1034rw, tb_c1034_9, tb_c1034_9n); tb_c1034rw += tb_c1034_9n; *tb_c1034rw = 0; (const char *)tb_c1034r; }));
            tb_o_Node * v_base = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1194))->q_pre_bases, v_j, sizeof(tb_o_Node *), 1194));
            if (({ const char *tb_s1035 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_base, 1195))->q_ty); ((unsigned char)tb_s1035[0] == 100 && (unsigned char)tb_s1035[1] == 105 && (unsigned char)tb_s1035[2] == 99 && (unsigned char)tb_s1035[3] == 116 && tb_s1035[4] == 0); })) {
                f_emit__put(v_c, "(void)tb_dpre1(");
                f_emit__emit_expr(v_c, v_base);
                f_emit__put(v_c, ({ const char *tb_c1036_0 = ", &"; long long tb_c1036_0n = sizeof(", &") - 1; const char *tb_c1036_1 = v_f; long long tb_c1036_1n = (long long)strlen(tb_c1036_1); const char *tb_c1036_2 = "); } }\n"; long long tb_c1036_2n = sizeof("); } }\n") - 1; char *tb_c1036r = tb_mem(tb_c1036_0n + tb_c1036_1n + tb_c1036_2n + 1), *tb_c1036rw = tb_c1036r; memcpy(tb_c1036rw, tb_c1036_0, tb_c1036_0n); tb_c1036rw += tb_c1036_0n; memcpy(tb_c1036rw, tb_c1036_1, tb_c1036_1n); tb_c1036rw += tb_c1036_1n; memcpy(tb_c1036rw, tb_c1036_2, tb_c1036_2n); tb_c1036rw += tb_c1036_2n; *tb_c1036rw = 0; (const char *)tb_c1036r; }));
            } else {
                f_emit__put(v_c, "tb_lpre(");
                f_emit__emit_expr(v_c, v_base);
                f_emit__put(v_c, ({ const char *tb_c1037_0 = ", "; long long tb_c1037_0n = sizeof(", ") - 1; const char *tb_c1037_1 = v_f; long long tb_c1037_1n = (long long)strlen(tb_c1037_1); const char *tb_c1037_2 = "); } }\n"; long long tb_c1037_2n = sizeof("); } }\n") - 1; char *tb_c1037r = tb_mem(tb_c1037_0n + tb_c1037_1n + tb_c1037_2n + 1), *tb_c1037rw = tb_c1037r; memcpy(tb_c1037rw, tb_c1037_0, tb_c1037_0n); tb_c1037rw += tb_c1037_0n; memcpy(tb_c1037rw, tb_c1037_1, tb_c1037_1n); tb_c1037rw += tb_c1037_1n; memcpy(tb_c1037rw, tb_c1037_2, tb_c1037_2n); tb_c1037rw += tb_c1037_2n; *tb_c1037rw = 0; (const char *)tb_c1037r; }));
            }
        }
    }
}

static _Bool f_emit__short_literal(tb_o_Node * v_n) {
    if (((({ const char *tb_s1038 = ((tb_o_Node *)tb_nn(v_n, 1206))->q_kind; !((unsigned char)tb_s1038[0] == 115 && (unsigned char)tb_s1038[1] == 116 && (unsigned char)tb_s1038[2] == 114 && (unsigned char)tb_s1038[3] == 105 && (unsigned char)tb_s1038[4] == 110 && (unsigned char)tb_s1038[5] == 103 && tb_s1038[6] == 0); }) || (tb_tlen(((tb_o_Node *)tb_nn(v_n, 1206))->q_value) < 2LL)) || (tb_tlen(((tb_o_Node *)tb_nn(v_n, 1206))->q_value) > 10LL))) {
        return 0;
    }
    {
        const char * tb_l1039 = ((tb_o_Node *)tb_nn(v_n, 1208))->q_value;
        for (long long tb_i1039 = 0; tb_l1039[tb_i1039] != 0; tb_i1039++) {
            const char * const v_ch = tb_chr((unsigned char)tb_l1039[tb_i1039]);
            if ((strcmp(v_ch, "\\") == 0)) {
                return 0;
            }
        }
    }
    return 1;
    tb_crash(1205, "функция short_literal дошла до конца без return");
    return 0;
}

static void f_emit__emit_count(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    if (((tb_o_Node *)tb_nn(v_n, 1214))->q_is_par) {
        f_emit__emit_par(v_c, v_n, v_depth);
        return;
    }
    { long long tb_v1040 = 1LL; long long *tb_p1040 = &(((tb_o_Comp *)tb_nn(v_c, 1217))->q_label); *tb_p1040 += tb_v1040; }
    const char * v_id = ({ const char *tb_c1041_0 = ""; long long tb_c1041_0n = sizeof("") - 1; char tb_c1041_1d[24]; long long tb_c1041_1n; const char *tb_c1041_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1218))->q_label, tb_c1041_1d, &tb_c1041_1n); char *tb_c1041r = tb_mem(tb_c1041_0n + tb_c1041_1n + 1), *tb_c1041rw = tb_c1041r; memcpy(tb_c1041rw, tb_c1041_0, tb_c1041_0n); tb_c1041rw += tb_c1041_0n; memcpy(tb_c1041rw, tb_c1041_1, tb_c1041_1n); tb_c1041rw += tb_c1041_1n; *tb_c1041rw = 0; (const char *)tb_c1041r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c1042_0 = "long long tb_end"; long long tb_c1042_0n = sizeof("long long tb_end") - 1; const char *tb_c1042_1 = v_id; long long tb_c1042_1n = (long long)strlen(tb_c1042_1); const char *tb_c1042_2 = " = "; long long tb_c1042_2n = sizeof(" = ") - 1; char *tb_c1042r = tb_mem(tb_c1042_0n + tb_c1042_1n + tb_c1042_2n + 1), *tb_c1042rw = tb_c1042r; memcpy(tb_c1042rw, tb_c1042_0, tb_c1042_0n); tb_c1042rw += tb_c1042_0n; memcpy(tb_c1042rw, tb_c1042_1, tb_c1042_1n); tb_c1042rw += tb_c1042_1n; memcpy(tb_c1042rw, tb_c1042_2, tb_c1042_2n); tb_c1042rw += tb_c1042_2n; *tb_c1042rw = 0; (const char *)tb_c1042r; }));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1223))->q_kids, 1LL, sizeof(tb_o_Node *), 1223)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c1043_0 = "for (long long v_"; long long tb_c1043_0n = sizeof("for (long long v_") - 1; const char *tb_c1043_1 = ((tb_o_Node *)tb_nn(v_n, 1226))->q_name; long long tb_c1043_1n = (long long)strlen(tb_c1043_1); const char *tb_c1043_2 = " = "; long long tb_c1043_2n = sizeof(" = ") - 1; char *tb_c1043r = tb_mem(tb_c1043_0n + tb_c1043_1n + tb_c1043_2n + 1), *tb_c1043rw = tb_c1043r; memcpy(tb_c1043rw, tb_c1043_0, tb_c1043_0n); tb_c1043rw += tb_c1043_0n; memcpy(tb_c1043rw, tb_c1043_1, tb_c1043_1n); tb_c1043rw += tb_c1043_1n; memcpy(tb_c1043rw, tb_c1043_2, tb_c1043_2n); tb_c1043rw += tb_c1043_2n; *tb_c1043rw = 0; (const char *)tb_c1043r; }));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1227))->q_kids, 0LL, sizeof(tb_o_Node *), 1227)));
    f_emit__put(v_c, ({ const char *tb_c1044_0 = "; v_"; long long tb_c1044_0n = sizeof("; v_") - 1; const char *tb_c1044_1 = ((tb_o_Node *)tb_nn(v_n, 1228))->q_name; long long tb_c1044_1n = (long long)strlen(tb_c1044_1); const char *tb_c1044_2 = " <= tb_end"; long long tb_c1044_2n = sizeof(" <= tb_end") - 1; const char *tb_c1044_3 = v_id; long long tb_c1044_3n = (long long)strlen(tb_c1044_3); const char *tb_c1044_4 = "; v_"; long long tb_c1044_4n = sizeof("; v_") - 1; const char *tb_c1044_5 = ((tb_o_Node *)tb_nn(v_n, 1228))->q_name; long long tb_c1044_5n = (long long)strlen(tb_c1044_5); const char *tb_c1044_6 = "++) {\n"; long long tb_c1044_6n = sizeof("++) {\n") - 1; char *tb_c1044r = tb_mem(tb_c1044_0n + tb_c1044_1n + tb_c1044_2n + tb_c1044_3n + tb_c1044_4n + tb_c1044_5n + tb_c1044_6n + 1), *tb_c1044rw = tb_c1044r; memcpy(tb_c1044rw, tb_c1044_0, tb_c1044_0n); tb_c1044rw += tb_c1044_0n; memcpy(tb_c1044rw, tb_c1044_1, tb_c1044_1n); tb_c1044rw += tb_c1044_1n; memcpy(tb_c1044rw, tb_c1044_2, tb_c1044_2n); tb_c1044rw += tb_c1044_2n; memcpy(tb_c1044rw, tb_c1044_3, tb_c1044_3n); tb_c1044rw += tb_c1044_3n; memcpy(tb_c1044rw, tb_c1044_4, tb_c1044_4n); tb_c1044rw += tb_c1044_4n; memcpy(tb_c1044rw, tb_c1044_5, tb_c1044_5n); tb_c1044rw += tb_c1044_5n; memcpy(tb_c1044rw, tb_c1044_6, tb_c1044_6n); tb_c1044rw += tb_c1044_6n; *tb_c1044rw = 0; (const char *)tb_c1044r; }));
    f_emit__emit_prefetch(v_c, v_n, (v_depth + 2LL), "", ({ const char *tb_c1045_0 = "v_"; long long tb_c1045_0n = sizeof("v_") - 1; const char *tb_c1045_1 = ((tb_o_Node *)tb_nn(v_n, 1229))->q_name; long long tb_c1045_1n = (long long)strlen(tb_c1045_1); char *tb_c1045r = tb_mem(tb_c1045_0n + tb_c1045_1n + 1), *tb_c1045rw = tb_c1045r; memcpy(tb_c1045rw, tb_c1045_0, tb_c1045_0n); tb_c1045rw += tb_c1045_0n; memcpy(tb_c1045rw, tb_c1045_1, tb_c1045_1n); tb_c1045rw += tb_c1045_1n; *tb_c1045rw = 0; (const char *)tb_c1045r; }), "long long");
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1230))->q_kids, 2LL, sizeof(tb_o_Node *), 1230)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_gives_return(tb_o_Comp * v_c, long long v_depth) {
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1238))->q_cur_func, 1238))->q_kids, 2LL, sizeof(tb_o_Node *), 1238));
    f_emit__indent(v_c, v_depth);
    if (((((tb_o_Node *)tb_nn(v_outputs, 1240))->q_kids)->len == 1LL)) {
        f_emit__put(v_c, ({ const char *tb_c1046_0 = "return v_"; long long tb_c1046_0n = sizeof("return v_") - 1; const char *tb_c1046_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1241))->q_kids, 0LL, sizeof(tb_o_Node *), 1241)), 1241))->q_name; long long tb_c1046_1n = (long long)strlen(tb_c1046_1); const char *tb_c1046_2 = ";\n"; long long tb_c1046_2n = sizeof(";\n") - 1; char *tb_c1046r = tb_mem(tb_c1046_0n + tb_c1046_1n + tb_c1046_2n + 1), *tb_c1046rw = tb_c1046r; memcpy(tb_c1046rw, tb_c1046_0, tb_c1046_0n); tb_c1046rw += tb_c1046_0n; memcpy(tb_c1046rw, tb_c1046_1, tb_c1046_1n); tb_c1046rw += tb_c1046_1n; memcpy(tb_c1046rw, tb_c1046_2, tb_c1046_2n); tb_c1046rw += tb_c1046_2n; *tb_c1046rw = 0; (const char *)tb_c1046r; }));
        return;
    }
    f_emit__put(v_c, ({ const char *tb_c1047_0 = "return ("; long long tb_c1047_0n = sizeof("return (") - 1; const char *tb_c1047_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1243))->q_cur_func, 1243))->q_ty); long long tb_c1047_1n = (long long)strlen(tb_c1047_1); const char *tb_c1047_2 = "){"; long long tb_c1047_2n = sizeof("){") - 1; char *tb_c1047r = tb_mem(tb_c1047_0n + tb_c1047_1n + tb_c1047_2n + 1), *tb_c1047rw = tb_c1047r; memcpy(tb_c1047rw, tb_c1047_0, tb_c1047_0n); tb_c1047rw += tb_c1047_0n; memcpy(tb_c1047rw, tb_c1047_1, tb_c1047_1n); tb_c1047rw += tb_c1047_1n; memcpy(tb_c1047rw, tb_c1047_2, tb_c1047_2n); tb_c1047rw += tb_c1047_2n; *tb_c1047rw = 0; (const char *)tb_c1047r; }));
    {
        long long tb_end1048 = ((((tb_o_Node *)tb_nn(v_outputs, 1244))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1048; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1049_0 = "v_"; long long tb_c1049_0n = sizeof("v_") - 1; const char *tb_c1049_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1247))->q_kids, v_i, sizeof(tb_o_Node *), 1247)), 1247))->q_name; long long tb_c1049_1n = (long long)strlen(tb_c1049_1); char *tb_c1049r = tb_mem(tb_c1049_0n + tb_c1049_1n + 1), *tb_c1049rw = tb_c1049r; memcpy(tb_c1049rw, tb_c1049_0, tb_c1049_0n); tb_c1049rw += tb_c1049_0n; memcpy(tb_c1049rw, tb_c1049_1, tb_c1049_1n); tb_c1049rw += tb_c1049_1n; *tb_c1049rw = 0; (const char *)tb_c1049r; }));
        }
    }
    f_emit__put(v_c, "};\n");
}

static _Bool f_emit__fill_form(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_body = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1252))->q_kids, 1LL, sizeof(tb_o_Node *), 1252));
    if ((((((tb_o_Node *)tb_nn(v_body, 1253))->q_kids)->len != 1LL) || (strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_body, 1253))->q_kids, 0LL, sizeof(tb_o_Node *), 1253)), 1253))->q_kind, "expr_stmt") != 0))) {
        return 0;
    }
    tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_body, 1255))->q_kids, 0LL, sizeof(tb_o_Node *), 1255)), 1255))->q_kids, 0LL, sizeof(tb_o_Node *), 1255));
    if ((((({ const char *tb_s1050 = ((tb_o_Node *)tb_nn(v_e, 1256))->q_kind; !((unsigned char)tb_s1050[0] == 109 && (unsigned char)tb_s1050[1] == 101 && (unsigned char)tb_s1050[2] == 116 && (unsigned char)tb_s1050[3] == 104 && (unsigned char)tb_s1050[4] == 111 && (unsigned char)tb_s1050[5] == 100 && tb_s1050[6] == 0); }) || ({ const char *tb_s1051 = ((tb_o_Node *)tb_nn(v_e, 1256))->q_name; !((unsigned char)tb_s1051[0] == 97 && (unsigned char)tb_s1051[1] == 100 && (unsigned char)tb_s1051[2] == 100 && tb_s1051[3] == 0); })) || ({ const char *tb_s1052 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1256))->q_kids, 0LL, sizeof(tb_o_Node *), 1256)), 1256))->q_kind; !((unsigned char)tb_s1052[0] == 110 && (unsigned char)tb_s1052[1] == 97 && (unsigned char)tb_s1052[2] == 109 && (unsigned char)tb_s1052[3] == 101 && tb_s1052[4] == 0); })) || f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1256))->q_kids, 0LL, sizeof(tb_o_Node *), 1256)), 1256))->q_ty))) {
        return 0;
    }
    tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1258))->q_kids, 1LL, sizeof(tb_o_Node *), 1258)), 1258))->q_kids, 0LL, sizeof(tb_o_Node *), 1258));
    const char * v_k = ((tb_o_Node *)tb_nn(v_v, 1259))->q_kind;
    return (((({ const char *tb_s1053 = v_k; ((unsigned char)tb_s1053[0] == 110 && (unsigned char)tb_s1053[1] == 117 && (unsigned char)tb_s1053[2] == 109 && (unsigned char)tb_s1053[3] == 98 && (unsigned char)tb_s1053[4] == 101 && (unsigned char)tb_s1053[5] == 114 && tb_s1053[6] == 0); }) || ({ const char *tb_s1054 = v_k; ((unsigned char)tb_s1054[0] == 100 && (unsigned char)tb_s1054[1] == 101 && (unsigned char)tb_s1054[2] == 99 && (unsigned char)tb_s1054[3] == 105 && (unsigned char)tb_s1054[4] == 109 && (unsigned char)tb_s1054[5] == 97 && (unsigned char)tb_s1054[6] == 108 && tb_s1054[7] == 0); })) || ({ const char *tb_s1055 = v_k; ((unsigned char)tb_s1055[0] == 115 && (unsigned char)tb_s1055[1] == 116 && (unsigned char)tb_s1055[2] == 114 && (unsigned char)tb_s1055[3] == 105 && (unsigned char)tb_s1055[4] == 110 && (unsigned char)tb_s1055[5] == 103 && tb_s1055[6] == 0); })) || ({ const char *tb_s1056 = v_k; ((unsigned char)tb_s1056[0] == 98 && (unsigned char)tb_s1056[1] == 111 && (unsigned char)tb_s1056[2] == 111 && (unsigned char)tb_s1056[3] == 108 && tb_s1056[4] == 0); }));
    tb_crash(1251, "функция fill_form дошла до конца без return");
    return 0;
}

static void f_emit__emit_statement(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1263))->q_kind;
    if (({ const char *tb_s1057 = v_k; ((unsigned char)tb_s1057[0] == 97 && (unsigned char)tb_s1057[1] == 115 && (unsigned char)tb_s1057[2] == 115 && (unsigned char)tb_s1057[3] == 105 && (unsigned char)tb_s1057[4] == 103 && (unsigned char)tb_s1057[5] == 110 && tb_s1057[6] == 0); })) {
        f_emit__emit_assign(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1058 = v_k; ((unsigned char)tb_s1058[0] == 115 && (unsigned char)tb_s1058[1] == 101 && (unsigned char)tb_s1058[2] == 116 && tb_s1058[3] == 0); })) {
        f_emit__emit_set(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1059 = v_k; ((unsigned char)tb_s1059[0] == 108 && (unsigned char)tb_s1059[1] == 105 && (unsigned char)tb_s1059[2] == 110 && (unsigned char)tb_s1059[3] == 107 && (unsigned char)tb_s1059[4] == 100 && (unsigned char)tb_s1059[5] == 101 && (unsigned char)tb_s1059[6] == 99 && (unsigned char)tb_s1059[7] == 108 && tb_s1059[8] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, ({ const char *tb_c1060_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 1270))->q_ty); long long tb_c1060_0n = (long long)strlen(tb_c1060_0); const char *tb_c1060_1 = " *v_"; long long tb_c1060_1n = sizeof(" *v_") - 1; const char *tb_c1060_2 = ((tb_o_Node *)tb_nn(v_n, 1270))->q_name; long long tb_c1060_2n = (long long)strlen(tb_c1060_2); const char *tb_c1060_3 = " = &("; long long tb_c1060_3n = sizeof(" = &(") - 1; char *tb_c1060r = tb_mem(tb_c1060_0n + tb_c1060_1n + tb_c1060_2n + tb_c1060_3n + 1), *tb_c1060rw = tb_c1060r; memcpy(tb_c1060rw, tb_c1060_0, tb_c1060_0n); tb_c1060rw += tb_c1060_0n; memcpy(tb_c1060rw, tb_c1060_1, tb_c1060_1n); tb_c1060rw += tb_c1060_1n; memcpy(tb_c1060rw, tb_c1060_2, tb_c1060_2n); tb_c1060rw += tb_c1060_2n; memcpy(tb_c1060rw, tb_c1060_3, tb_c1060_3n); tb_c1060rw += tb_c1060_3n; *tb_c1060rw = 0; (const char *)tb_c1060r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1271))->q_kids, 0LL, sizeof(tb_o_Node *), 1271)));
        f_emit__put(v_c, ");\n");
    } else if (({ const char *tb_s1061 = v_k; ((unsigned char)tb_s1061[0] == 109 && (unsigned char)tb_s1061[1] == 117 && (unsigned char)tb_s1061[2] == 108 && (unsigned char)tb_s1061[3] == 116 && (unsigned char)tb_s1061[4] == 105 && tb_s1061[5] == 0); })) {
        f_emit__emit_multi(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1062 = v_k; ((unsigned char)tb_s1062[0] == 105 && (unsigned char)tb_s1062[1] == 102 && tb_s1062[2] == 0); })) {
        f_emit__emit_if(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1063 = v_k; ((unsigned char)tb_s1063[0] == 101 && (unsigned char)tb_s1063[1] == 97 && (unsigned char)tb_s1063[2] == 99 && (unsigned char)tb_s1063[3] == 104 && tb_s1063[4] == 0); })) {
        f_emit__emit_each(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1064 = v_k; ((unsigned char)tb_s1064[0] == 99 && (unsigned char)tb_s1064[1] == 111 && (unsigned char)tb_s1064[2] == 117 && (unsigned char)tb_s1064[3] == 110 && (unsigned char)tb_s1064[4] == 116 && tb_s1064[5] == 0); })) {
        f_emit__emit_count(v_c, v_n, v_depth);
    } else if (((strcmp(v_k, "repeat_times") == 0) && f_emit__fill_form(v_c, v_n))) {
        tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1283))->q_kids, 1LL, sizeof(tb_o_Node *), 1283)), 1283))->q_kids, 0LL, sizeof(tb_o_Node *), 1283)), 1283))->q_kids, 0LL, sizeof(tb_o_Node *), 1283));
        tb_o_Node * v_lst = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1284))->q_kids, 0LL, sizeof(tb_o_Node *), 1284));
        tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1285))->q_kids, 1LL, sizeof(tb_o_Node *), 1285)), 1285))->q_kids, 0LL, sizeof(tb_o_Node *), 1285));
        f_emit__indent(v_c, v_depth);
        if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_lst, 1287))->q_ty)) {
            f_emit__put(v_c, "tb_bfill(");
            f_emit__emit_expr(v_c, v_lst);
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1291))->q_kids, 0LL, sizeof(tb_o_Node *), 1291)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, v_value);
            f_emit__put(v_c, ");\n");
        } else {
            long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1296))->q_types, ((tb_o_Node *)tb_nn(v_lst, 1296))->q_ty, sizeof(tb_o_Type *), 1296)), 1296))->q_elem;
            f_emit__put(v_c, "tb_fill(");
            f_emit__emit_expr(v_c, v_lst);
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1300))->q_kids, 0LL, sizeof(tb_o_Node *), 1300)));
            f_emit__put(v_c, ({ const char *tb_c1065_0 = ", &("; long long tb_c1065_0n = sizeof(", &(") - 1; const char *tb_c1065_1 = f_emit__c_type(v_c, v_elem); long long tb_c1065_1n = (long long)strlen(tb_c1065_1); const char *tb_c1065_2 = "){"; long long tb_c1065_2n = sizeof("){") - 1; char *tb_c1065r = tb_mem(tb_c1065_0n + tb_c1065_1n + tb_c1065_2n + 1), *tb_c1065rw = tb_c1065r; memcpy(tb_c1065rw, tb_c1065_0, tb_c1065_0n); tb_c1065rw += tb_c1065_0n; memcpy(tb_c1065rw, tb_c1065_1, tb_c1065_1n); tb_c1065rw += tb_c1065_1n; memcpy(tb_c1065rw, tb_c1065_2, tb_c1065_2n); tb_c1065rw += tb_c1065_2n; *tb_c1065rw = 0; (const char *)tb_c1065r; }));
            f_emit__emit_as(v_c, v_value, v_elem);
            f_emit__put(v_c, "});\n");
        }
    } else if ((strcmp(v_k, "repeat_times") == 0)) {
        { long long tb_v1066 = 1LL; long long *tb_p1066 = &(((tb_o_Comp *)tb_nn(v_c, 1305))->q_label); *tb_p1066 += tb_v1066; }
        const char * v_id = ({ const char *tb_c1067_0 = ""; long long tb_c1067_0n = sizeof("") - 1; char tb_c1067_1d[24]; long long tb_c1067_1n; const char *tb_c1067_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1306))->q_label, tb_c1067_1d, &tb_c1067_1n); char *tb_c1067r = tb_mem(tb_c1067_0n + tb_c1067_1n + 1), *tb_c1067rw = tb_c1067r; memcpy(tb_c1067rw, tb_c1067_0, tb_c1067_0n); tb_c1067rw += tb_c1067_0n; memcpy(tb_c1067rw, tb_c1067_1, tb_c1067_1n); tb_c1067rw += tb_c1067_1n; *tb_c1067rw = 0; (const char *)tb_c1067r; });
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, ({ const char *tb_c1068_0 = "for (long long tb_r"; long long tb_c1068_0n = sizeof("for (long long tb_r") - 1; const char *tb_c1068_1 = v_id; long long tb_c1068_1n = (long long)strlen(tb_c1068_1); const char *tb_c1068_2 = " = 0, tb_n"; long long tb_c1068_2n = sizeof(" = 0, tb_n") - 1; const char *tb_c1068_3 = v_id; long long tb_c1068_3n = (long long)strlen(tb_c1068_3); const char *tb_c1068_4 = " = "; long long tb_c1068_4n = sizeof(" = ") - 1; char *tb_c1068r = tb_mem(tb_c1068_0n + tb_c1068_1n + tb_c1068_2n + tb_c1068_3n + tb_c1068_4n + 1), *tb_c1068rw = tb_c1068r; memcpy(tb_c1068rw, tb_c1068_0, tb_c1068_0n); tb_c1068rw += tb_c1068_0n; memcpy(tb_c1068rw, tb_c1068_1, tb_c1068_1n); tb_c1068rw += tb_c1068_1n; memcpy(tb_c1068rw, tb_c1068_2, tb_c1068_2n); tb_c1068rw += tb_c1068_2n; memcpy(tb_c1068rw, tb_c1068_3, tb_c1068_3n); tb_c1068rw += tb_c1068_3n; memcpy(tb_c1068rw, tb_c1068_4, tb_c1068_4n); tb_c1068rw += tb_c1068_4n; *tb_c1068rw = 0; (const char *)tb_c1068r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1309))->q_kids, 0LL, sizeof(tb_o_Node *), 1309)));
        f_emit__put(v_c, ({ const char *tb_c1069_0 = "; tb_r"; long long tb_c1069_0n = sizeof("; tb_r") - 1; const char *tb_c1069_1 = v_id; long long tb_c1069_1n = (long long)strlen(tb_c1069_1); const char *tb_c1069_2 = " < tb_n"; long long tb_c1069_2n = sizeof(" < tb_n") - 1; const char *tb_c1069_3 = v_id; long long tb_c1069_3n = (long long)strlen(tb_c1069_3); const char *tb_c1069_4 = "; tb_r"; long long tb_c1069_4n = sizeof("; tb_r") - 1; const char *tb_c1069_5 = v_id; long long tb_c1069_5n = (long long)strlen(tb_c1069_5); const char *tb_c1069_6 = "++) {\n"; long long tb_c1069_6n = sizeof("++) {\n") - 1; char *tb_c1069r = tb_mem(tb_c1069_0n + tb_c1069_1n + tb_c1069_2n + tb_c1069_3n + tb_c1069_4n + tb_c1069_5n + tb_c1069_6n + 1), *tb_c1069rw = tb_c1069r; memcpy(tb_c1069rw, tb_c1069_0, tb_c1069_0n); tb_c1069rw += tb_c1069_0n; memcpy(tb_c1069rw, tb_c1069_1, tb_c1069_1n); tb_c1069rw += tb_c1069_1n; memcpy(tb_c1069rw, tb_c1069_2, tb_c1069_2n); tb_c1069rw += tb_c1069_2n; memcpy(tb_c1069rw, tb_c1069_3, tb_c1069_3n); tb_c1069rw += tb_c1069_3n; memcpy(tb_c1069rw, tb_c1069_4, tb_c1069_4n); tb_c1069rw += tb_c1069_4n; memcpy(tb_c1069rw, tb_c1069_5, tb_c1069_5n); tb_c1069rw += tb_c1069_5n; memcpy(tb_c1069rw, tb_c1069_6, tb_c1069_6n); tb_c1069rw += tb_c1069_6n; *tb_c1069rw = 0; (const char *)tb_c1069r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1311))->q_kids, 1LL, sizeof(tb_o_Node *), 1311)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if ((strcmp(v_k, "repeat_while") == 0)) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "while (");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1317))->q_kids, 0LL, sizeof(tb_o_Node *), 1317)));
        f_emit__put(v_c, ") {\n");
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1319))->q_kids, 1LL, sizeof(tb_o_Node *), 1319)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if (({ const char *tb_s1070 = v_k; ((unsigned char)tb_s1070[0] == 116 && (unsigned char)tb_s1070[1] == 111 && (unsigned char)tb_s1070[2] == 103 && (unsigned char)tb_s1070[3] == 101 && (unsigned char)tb_s1070[4] == 116 && (unsigned char)tb_s1070[5] == 104 && (unsigned char)tb_s1070[6] == 101 && (unsigned char)tb_s1070[7] == 114 && tb_s1070[8] == 0); })) {
        f_emit__emit_together(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1071 = v_k; ((unsigned char)tb_s1071[0] == 116 && (unsigned char)tb_s1071[1] == 101 && (unsigned char)tb_s1071[2] == 109 && (unsigned char)tb_s1071[3] == 112 && tb_s1071[4] == 0); })) {
        { long long tb_v1072 = 1LL; long long *tb_p1072 = &(((tb_o_Comp *)tb_nn(v_c, 1325))->q_label); *tb_p1072 += tb_v1072; }
        const char * v_id = ({ const char *tb_c1073_0 = ""; long long tb_c1073_0n = sizeof("") - 1; char tb_c1073_1d[24]; long long tb_c1073_1n; const char *tb_c1073_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1326))->q_label, tb_c1073_1d, &tb_c1073_1n); char *tb_c1073r = tb_mem(tb_c1073_0n + tb_c1073_1n + 1), *tb_c1073rw = tb_c1073r; memcpy(tb_c1073rw, tb_c1073_0, tb_c1073_0n); tb_c1073rw += tb_c1073_0n; memcpy(tb_c1073rw, tb_c1073_1, tb_c1073_1n); tb_c1073rw += tb_c1073_1n; *tb_c1073rw = 0; (const char *)tb_c1073r; });
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "{\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c1074_0 = "tb_mark tb_k"; long long tb_c1074_0n = sizeof("tb_mark tb_k") - 1; const char *tb_c1074_1 = v_id; long long tb_c1074_1n = (long long)strlen(tb_c1074_1); const char *tb_c1074_2 = " = tb_temp_begin();\n"; long long tb_c1074_2n = sizeof(" = tb_temp_begin();\n") - 1; char *tb_c1074r = tb_mem(tb_c1074_0n + tb_c1074_1n + tb_c1074_2n + 1), *tb_c1074rw = tb_c1074r; memcpy(tb_c1074rw, tb_c1074_0, tb_c1074_0n); tb_c1074rw += tb_c1074_0n; memcpy(tb_c1074rw, tb_c1074_1, tb_c1074_1n); tb_c1074rw += tb_c1074_1n; memcpy(tb_c1074rw, tb_c1074_2, tb_c1074_2n); tb_c1074rw += tb_c1074_2n; *tb_c1074rw = 0; (const char *)tb_c1074r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1331))->q_kids, 0LL, sizeof(tb_o_Node *), 1331)), (v_depth + 1LL));
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c1075_0 = "tb_temp_end(tb_k"; long long tb_c1075_0n = sizeof("tb_temp_end(tb_k") - 1; const char *tb_c1075_1 = v_id; long long tb_c1075_1n = (long long)strlen(tb_c1075_1); const char *tb_c1075_2 = ");\n"; long long tb_c1075_2n = sizeof(");\n") - 1; char *tb_c1075r = tb_mem(tb_c1075_0n + tb_c1075_1n + tb_c1075_2n + 1), *tb_c1075rw = tb_c1075r; memcpy(tb_c1075rw, tb_c1075_0, tb_c1075_0n); tb_c1075rw += tb_c1075_0n; memcpy(tb_c1075rw, tb_c1075_1, tb_c1075_1n); tb_c1075rw += tb_c1075_1n; memcpy(tb_c1075rw, tb_c1075_2, tb_c1075_2n); tb_c1075rw += tb_c1075_2n; *tb_c1075rw = 0; (const char *)tb_c1075r; }));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if (({ const char *tb_s1076 = v_k; ((unsigned char)tb_s1076[0] == 115 && (unsigned char)tb_s1076[1] == 107 && (unsigned char)tb_s1076[2] == 105 && (unsigned char)tb_s1076[3] == 112 && tb_s1076[4] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "continue;\n");
    } else if (({ const char *tb_s1077 = v_k; ((unsigned char)tb_s1077[0] == 115 && (unsigned char)tb_s1077[1] == 116 && (unsigned char)tb_s1077[2] == 111 && (unsigned char)tb_s1077[3] == 112 && tb_s1077[4] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "break;\n");
    } else if (({ const char *tb_s1078 = v_k; ((unsigned char)tb_s1078[0] == 102 && (unsigned char)tb_s1078[1] == 97 && (unsigned char)tb_s1078[2] == 105 && (unsigned char)tb_s1078[3] == 108 && tb_s1078[4] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "{ tb_err = ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1345))->q_kids, 0LL, sizeof(tb_o_Node *), 1345)));
        f_emit__put(v_c, "; ");
        f_emit__emit_bail(v_c);
        f_emit__put(v_c, " }\n");
    } else if (({ const char *tb_s1079 = v_k; ((unsigned char)tb_s1079[0] == 114 && (unsigned char)tb_s1079[1] == 101 && (unsigned char)tb_s1079[2] == 116 && (unsigned char)tb_s1079[3] == 117 && (unsigned char)tb_s1079[4] == 114 && (unsigned char)tb_s1079[5] == 110 && tb_s1079[6] == 0); })) {
        if (((((tb_o_Node *)tb_nn(v_n, 1350))->q_kids)->len == 1LL)) {
            f_emit__emit_pre(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1351))->q_kids, 0LL, sizeof(tb_o_Node *), 1351)), v_depth);
        }
        if ((((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1352))->q_cur_func, 1352))->q_kids, 2LL, sizeof(tb_o_Node *), 1352)), 1352))->q_kids)->len > 0LL) && ((((tb_o_Node *)tb_nn(v_n, 1352))->q_kids)->len == 0LL))) {
            f_emit__emit_gives_return(v_c, v_depth);
        } else if (((((tb_o_Node *)tb_nn(v_n, 1354))->q_kids)->len == 1LL)) {
            f_emit__indent(v_c, v_depth);
            f_emit__put(v_c, "return ");
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1357))->q_kids, 0LL, sizeof(tb_o_Node *), 1357)), ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1357))->q_cur_func, 1357))->q_ty);
            f_emit__put(v_c, ";\n");
        } else {
            f_emit__indent(v_c, v_depth);
            f_emit__put(v_c, "return;\n");
        }
    } else if ((strcmp(v_k, "expr_stmt") == 0)) {
        tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1363))->q_kids, 0LL, sizeof(tb_o_Node *), 1363));
        if ((({ const char *tb_s1080 = ((tb_o_Node *)tb_nn(v_e, 1364))->q_kind; ((unsigned char)tb_s1080[0] == 116 && (unsigned char)tb_s1080[1] == 114 && (unsigned char)tb_s1080[2] == 121 && tb_s1080[3] == 0); }) || ({ const char *tb_s1081 = ((tb_o_Node *)tb_nn(v_e, 1364))->q_kind; ((unsigned char)tb_s1081[0] == 101 && (unsigned char)tb_s1081[1] == 108 && (unsigned char)tb_s1081[2] == 115 && (unsigned char)tb_s1081[3] == 101 && (unsigned char)tb_s1081[4] == 95 && (unsigned char)tb_s1081[5] == 101 && (unsigned char)tb_s1081[6] == 114 && (unsigned char)tb_s1081[7] == 114 && tb_s1081[8] == 0); }))) {
            f_emit__emit_pre(v_c, v_e, v_depth);
            return;
        }
        f_emit__indent(v_c, v_depth);
        if ((({ const char *tb_s1082 = ((tb_o_Node *)tb_nn(v_e, 1368))->q_kind; ((unsigned char)tb_s1082[0] == 109 && (unsigned char)tb_s1082[1] == 101 && (unsigned char)tb_s1082[2] == 116 && (unsigned char)tb_s1082[3] == 104 && (unsigned char)tb_s1082[4] == 111 && (unsigned char)tb_s1082[5] == 100 && tb_s1082[6] == 0); }) && ({ const char *tb_s1083 = ((tb_o_Node *)tb_nn(v_e, 1368))->q_name; ((unsigned char)tb_s1083[0] == 97 && (unsigned char)tb_s1083[1] == 100 && (unsigned char)tb_s1083[2] == 100 && tb_s1083[3] == 0); }))) {
            long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1370))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1370))->q_kids, 0LL, sizeof(tb_o_Node *), 1370)), 1370))->q_ty, sizeof(tb_o_Type *), 1370)), 1370))->q_elem;
            { long long tb_v1084 = 1LL; long long *tb_p1084 = &(((tb_o_Comp *)tb_nn(v_c, 1371))->q_label); *tb_p1084 += tb_v1084; }
            const char * v_id = ({ const char *tb_c1085_0 = ""; long long tb_c1085_0n = sizeof("") - 1; char tb_c1085_1d[24]; long long tb_c1085_1n; const char *tb_c1085_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1372))->q_label, tb_c1085_1d, &tb_c1085_1n); char *tb_c1085r = tb_mem(tb_c1085_0n + tb_c1085_1n + 1), *tb_c1085rw = tb_c1085r; memcpy(tb_c1085rw, tb_c1085_0, tb_c1085_0n); tb_c1085rw += tb_c1085_0n; memcpy(tb_c1085rw, tb_c1085_1, tb_c1085_1n); tb_c1085rw += tb_c1085_1n; *tb_c1085rw = 0; (const char *)tb_c1085r; });
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1373))->q_kids, 1LL, sizeof(tb_o_Node *), 1373)), 1373))->q_kids, 0LL, sizeof(tb_o_Node *), 1373));
            if ((((f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1374))->q_kids, 0LL, sizeof(tb_o_Node *), 1374)), 1374))->q_ty) && ({ const char *tb_s1086 = ((tb_o_Node *)tb_nn(v_value, 1374))->q_kind; ((unsigned char)tb_s1086[0] == 99 && (unsigned char)tb_s1086[1] == 97 && (unsigned char)tb_s1086[2] == 108 && (unsigned char)tb_s1086[3] == 108 && tb_s1086[4] == 0); })) && ((tb_o_Node *)tb_nn(v_value, 1374))->q_has_ref) && ({ const char *tb_s1087 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_value, 1374))->q_ref, 1374))->q_kind; ((unsigned char)tb_s1087[0] == 111 && (unsigned char)tb_s1087[1] == 98 && (unsigned char)tb_s1087[2] == 106 && tb_s1087[3] == 0); }))) {
                tb_o_Node * v_o = ((tb_o_Node *)tb_nn(v_value, 1376))->q_ref;
                f_emit__put(v_c, ({ const char *tb_c1088_0 = "tb_colpush_"; long long tb_c1088_0n = sizeof("tb_colpush_") - 1; const char *tb_c1088_1 = ((tb_o_Node *)tb_nn(v_o, 1377))->q_name; long long tb_c1088_1n = (long long)strlen(tb_c1088_1); const char *tb_c1088_2 = "("; long long tb_c1088_2n = sizeof("(") - 1; char *tb_c1088r = tb_mem(tb_c1088_0n + tb_c1088_1n + tb_c1088_2n + 1), *tb_c1088rw = tb_c1088r; memcpy(tb_c1088rw, tb_c1088_0, tb_c1088_0n); tb_c1088rw += tb_c1088_0n; memcpy(tb_c1088rw, tb_c1088_1, tb_c1088_1n); tb_c1088rw += tb_c1088_1n; memcpy(tb_c1088rw, tb_c1088_2, tb_c1088_2n); tb_c1088rw += tb_c1088_2n; *tb_c1088rw = 0; (const char *)tb_c1088r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1378))->q_kids, 0LL, sizeof(tb_o_Node *), 1378)));
                {
                    long long tb_end1089 = ((((tb_o_Node *)tb_nn(v_o, 1379))->q_kids)->len - 1LL);
                    for (long long v_i = 0LL; v_i <= tb_end1089; v_i++) {
                        f_emit__put(v_c, ", ");
                        if (({ const char *tb_s1090 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 1381))->q_kids, v_i, sizeof(tb_o_Node *), 1381)), 1381))->q_kind; ((unsigned char)tb_s1090[0] == 109 && (unsigned char)tb_s1090[1] == 105 && (unsigned char)tb_s1090[2] == 115 && (unsigned char)tb_s1090[3] == 115 && (unsigned char)tb_s1090[4] == 105 && (unsigned char)tb_s1090[5] == 110 && (unsigned char)tb_s1090[6] == 103 && tb_s1090[7] == 0); })) {
                            f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1382))->q_kids, v_i, sizeof(tb_o_Node *), 1382)), 1382))->q_ty);
                        } else {
                            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 1384))->q_kids, v_i, sizeof(tb_o_Node *), 1384)), 1384))->q_kids, 0LL, sizeof(tb_o_Node *), 1384)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1384))->q_kids, v_i, sizeof(tb_o_Node *), 1384)), 1384))->q_ty);
                        }
                    }
                }
                f_emit__put(v_c, ({ const char *tb_c1091_0 = ", "; long long tb_c1091_0n = sizeof(", ") - 1; char tb_c1091_1d[24]; long long tb_c1091_1n; const char *tb_c1091_1 = tb_digits(((tb_o_Node *)tb_nn(v_e, 1385))->q_line, tb_c1091_1d, &tb_c1091_1n); const char *tb_c1091_2 = ");\n"; long long tb_c1091_2n = sizeof(");\n") - 1; char *tb_c1091r = tb_mem(tb_c1091_0n + tb_c1091_1n + tb_c1091_2n + 1), *tb_c1091rw = tb_c1091r; memcpy(tb_c1091rw, tb_c1091_0, tb_c1091_0n); tb_c1091rw += tb_c1091_0n; memcpy(tb_c1091rw, tb_c1091_1, tb_c1091_1n); tb_c1091rw += tb_c1091_1n; memcpy(tb_c1091rw, tb_c1091_2, tb_c1091_2n); tb_c1091rw += tb_c1091_2n; *tb_c1091rw = 0; (const char *)tb_c1091r; }));
                return;
            }
            if (f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1387))->q_kids, 0LL, sizeof(tb_o_Node *), 1387)), 1387))->q_ty)) {
                f_emit__put(v_c, ({ const char *tb_c1092_0 = "{ "; long long tb_c1092_0n = sizeof("{ ") - 1; const char *tb_c1092_1 = f_emit__c_type(v_c, v_elem); long long tb_c1092_1n = (long long)strlen(tb_c1092_1); const char *tb_c1092_2 = " tb_v"; long long tb_c1092_2n = sizeof(" tb_v") - 1; const char *tb_c1092_3 = v_id; long long tb_c1092_3n = (long long)strlen(tb_c1092_3); const char *tb_c1092_4 = " = "; long long tb_c1092_4n = sizeof(" = ") - 1; char *tb_c1092r = tb_mem(tb_c1092_0n + tb_c1092_1n + tb_c1092_2n + tb_c1092_3n + tb_c1092_4n + 1), *tb_c1092rw = tb_c1092r; memcpy(tb_c1092rw, tb_c1092_0, tb_c1092_0n); tb_c1092rw += tb_c1092_0n; memcpy(tb_c1092rw, tb_c1092_1, tb_c1092_1n); tb_c1092rw += tb_c1092_1n; memcpy(tb_c1092rw, tb_c1092_2, tb_c1092_2n); tb_c1092rw += tb_c1092_2n; memcpy(tb_c1092rw, tb_c1092_3, tb_c1092_3n); tb_c1092rw += tb_c1092_3n; memcpy(tb_c1092rw, tb_c1092_4, tb_c1092_4n); tb_c1092rw += tb_c1092_4n; *tb_c1092rw = 0; (const char *)tb_c1092r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1389))->q_kids, 1LL, sizeof(tb_o_Node *), 1389)), 1389))->q_kids, 0LL, sizeof(tb_o_Node *), 1389)));
                f_emit__put(v_c, ({ const char *tb_c1093_0 = "; tb_coladd_"; long long tb_c1093_0n = sizeof("; tb_coladd_") - 1; const char *tb_c1093_1 = f_emit__cols_name(v_c, v_elem); long long tb_c1093_1n = (long long)strlen(tb_c1093_1); const char *tb_c1093_2 = "("; long long tb_c1093_2n = sizeof("(") - 1; char *tb_c1093r = tb_mem(tb_c1093_0n + tb_c1093_1n + tb_c1093_2n + 1), *tb_c1093rw = tb_c1093r; memcpy(tb_c1093rw, tb_c1093_0, tb_c1093_0n); tb_c1093rw += tb_c1093_0n; memcpy(tb_c1093rw, tb_c1093_1, tb_c1093_1n); tb_c1093rw += tb_c1093_1n; memcpy(tb_c1093rw, tb_c1093_2, tb_c1093_2n); tb_c1093rw += tb_c1093_2n; *tb_c1093rw = 0; (const char *)tb_c1093r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1391))->q_kids, 0LL, sizeof(tb_o_Node *), 1391)));
                f_emit__put(v_c, ({ const char *tb_c1094_0 = ", tb_v"; long long tb_c1094_0n = sizeof(", tb_v") - 1; const char *tb_c1094_1 = v_id; long long tb_c1094_1n = (long long)strlen(tb_c1094_1); const char *tb_c1094_2 = ", "; long long tb_c1094_2n = sizeof(", ") - 1; char tb_c1094_3d[24]; long long tb_c1094_3n; const char *tb_c1094_3 = tb_digits(((tb_o_Node *)tb_nn(v_e, 1392))->q_line, tb_c1094_3d, &tb_c1094_3n); const char *tb_c1094_4 = "); }\n"; long long tb_c1094_4n = sizeof("); }\n") - 1; char *tb_c1094r = tb_mem(tb_c1094_0n + tb_c1094_1n + tb_c1094_2n + tb_c1094_3n + tb_c1094_4n + 1), *tb_c1094rw = tb_c1094r; memcpy(tb_c1094rw, tb_c1094_0, tb_c1094_0n); tb_c1094rw += tb_c1094_0n; memcpy(tb_c1094rw, tb_c1094_1, tb_c1094_1n); tb_c1094rw += tb_c1094_1n; memcpy(tb_c1094rw, tb_c1094_2, tb_c1094_2n); tb_c1094rw += tb_c1094_2n; memcpy(tb_c1094rw, tb_c1094_3, tb_c1094_3n); tb_c1094rw += tb_c1094_3n; memcpy(tb_c1094rw, tb_c1094_4, tb_c1094_4n); tb_c1094rw += tb_c1094_4n; *tb_c1094rw = 0; (const char *)tb_c1094r; }));
                return;
            }
            if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1394))->q_kids, 0LL, sizeof(tb_o_Node *), 1394)), 1394))->q_ty)) {
                f_emit__put(v_c, ({ const char *tb_c1095_0 = "{ _Bool tb_v"; long long tb_c1095_0n = sizeof("{ _Bool tb_v") - 1; const char *tb_c1095_1 = v_id; long long tb_c1095_1n = (long long)strlen(tb_c1095_1); const char *tb_c1095_2 = " = "; long long tb_c1095_2n = sizeof(" = ") - 1; char *tb_c1095r = tb_mem(tb_c1095_0n + tb_c1095_1n + tb_c1095_2n + 1), *tb_c1095rw = tb_c1095r; memcpy(tb_c1095rw, tb_c1095_0, tb_c1095_0n); tb_c1095rw += tb_c1095_0n; memcpy(tb_c1095rw, tb_c1095_1, tb_c1095_1n); tb_c1095rw += tb_c1095_1n; memcpy(tb_c1095rw, tb_c1095_2, tb_c1095_2n); tb_c1095rw += tb_c1095_2n; *tb_c1095rw = 0; (const char *)tb_c1095r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1396))->q_kids, 1LL, sizeof(tb_o_Node *), 1396)), 1396))->q_kids, 0LL, sizeof(tb_o_Node *), 1396)));
                f_emit__put(v_c, "; tb_badd(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1398))->q_kids, 0LL, sizeof(tb_o_Node *), 1398)));
                f_emit__put(v_c, ({ const char *tb_c1096_0 = ", tb_v"; long long tb_c1096_0n = sizeof(", tb_v") - 1; const char *tb_c1096_1 = v_id; long long tb_c1096_1n = (long long)strlen(tb_c1096_1); const char *tb_c1096_2 = "); }\n"; long long tb_c1096_2n = sizeof("); }\n") - 1; char *tb_c1096r = tb_mem(tb_c1096_0n + tb_c1096_1n + tb_c1096_2n + 1), *tb_c1096rw = tb_c1096r; memcpy(tb_c1096rw, tb_c1096_0, tb_c1096_0n); tb_c1096rw += tb_c1096_0n; memcpy(tb_c1096rw, tb_c1096_1, tb_c1096_1n); tb_c1096rw += tb_c1096_1n; memcpy(tb_c1096rw, tb_c1096_2, tb_c1096_2n); tb_c1096rw += tb_c1096_2n; *tb_c1096rw = 0; (const char *)tb_c1096r; }));
                return;
            }
            f_emit__put(v_c, ({ const char *tb_c1097_0 = "{ "; long long tb_c1097_0n = sizeof("{ ") - 1; const char *tb_c1097_1 = f_emit__c_type(v_c, v_elem); long long tb_c1097_1n = (long long)strlen(tb_c1097_1); const char *tb_c1097_2 = " tb_v"; long long tb_c1097_2n = sizeof(" tb_v") - 1; const char *tb_c1097_3 = v_id; long long tb_c1097_3n = (long long)strlen(tb_c1097_3); const char *tb_c1097_4 = " = "; long long tb_c1097_4n = sizeof(" = ") - 1; char *tb_c1097r = tb_mem(tb_c1097_0n + tb_c1097_1n + tb_c1097_2n + tb_c1097_3n + tb_c1097_4n + 1), *tb_c1097rw = tb_c1097r; memcpy(tb_c1097rw, tb_c1097_0, tb_c1097_0n); tb_c1097rw += tb_c1097_0n; memcpy(tb_c1097rw, tb_c1097_1, tb_c1097_1n); tb_c1097rw += tb_c1097_1n; memcpy(tb_c1097rw, tb_c1097_2, tb_c1097_2n); tb_c1097rw += tb_c1097_2n; memcpy(tb_c1097rw, tb_c1097_3, tb_c1097_3n); tb_c1097rw += tb_c1097_3n; memcpy(tb_c1097rw, tb_c1097_4, tb_c1097_4n); tb_c1097rw += tb_c1097_4n; *tb_c1097rw = 0; (const char *)tb_c1097r; }));
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1402))->q_kids, 1LL, sizeof(tb_o_Node *), 1402)), 1402))->q_kids, 0LL, sizeof(tb_o_Node *), 1402)), v_elem);
            f_emit__put(v_c, ({ const char *tb_c1098_0 = "; *("; long long tb_c1098_0n = sizeof("; *(") - 1; const char *tb_c1098_1 = f_emit__c_type(v_c, v_elem); long long tb_c1098_1n = (long long)strlen(tb_c1098_1); const char *tb_c1098_2 = " *)tb_grow("; long long tb_c1098_2n = sizeof(" *)tb_grow(") - 1; char *tb_c1098r = tb_mem(tb_c1098_0n + tb_c1098_1n + tb_c1098_2n + 1), *tb_c1098rw = tb_c1098r; memcpy(tb_c1098rw, tb_c1098_0, tb_c1098_0n); tb_c1098rw += tb_c1098_0n; memcpy(tb_c1098rw, tb_c1098_1, tb_c1098_1n); tb_c1098rw += tb_c1098_1n; memcpy(tb_c1098rw, tb_c1098_2, tb_c1098_2n); tb_c1098rw += tb_c1098_2n; *tb_c1098rw = 0; (const char *)tb_c1098r; }));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1404))->q_kids, 0LL, sizeof(tb_o_Node *), 1404)));
            f_emit__put(v_c, ({ const char *tb_c1099_0 = ") = tb_v"; long long tb_c1099_0n = sizeof(") = tb_v") - 1; const char *tb_c1099_1 = v_id; long long tb_c1099_1n = (long long)strlen(tb_c1099_1); const char *tb_c1099_2 = "; }\n"; long long tb_c1099_2n = sizeof("; }\n") - 1; char *tb_c1099r = tb_mem(tb_c1099_0n + tb_c1099_1n + tb_c1099_2n + 1), *tb_c1099rw = tb_c1099r; memcpy(tb_c1099rw, tb_c1099_0, tb_c1099_0n); tb_c1099rw += tb_c1099_0n; memcpy(tb_c1099rw, tb_c1099_1, tb_c1099_1n); tb_c1099rw += tb_c1099_1n; memcpy(tb_c1099rw, tb_c1099_2, tb_c1099_2n); tb_c1099rw += tb_c1099_2n; *tb_c1099rw = 0; (const char *)tb_c1099r; }));
        } else {
            f_emit__emit_expr(v_c, v_e);
            f_emit__put(v_c, ";\n");
        }
    }
}

static void f_emit__emit_block(tb_o_Comp * v_c, tb_o_Node * v_block, long long v_depth) {
    {
        tb_list * tb_l1100 = ((tb_o_Node *)tb_nn(v_block, 1411))->q_kids;
        for (long long tb_i1100 = 0; tb_i1100 < tb_l1100->len; tb_i1100++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l1100->data)[tb_i1100];
            f_emit__emit_statement(v_c, v_s, v_depth);
        }
    }
}

static void f_emit__emit_signature(tb_o_Comp * v_c, tb_o_Node * v_f) {
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1417))->q_kids, 0LL, sizeof(tb_o_Node *), 1417));
    f_emit__put(v_c, ({ const char *tb_c1101_0 = "static "; long long tb_c1101_0n = sizeof("static ") - 1; const char *tb_c1101_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1418))->q_ty); long long tb_c1101_1n = (long long)strlen(tb_c1101_1); const char *tb_c1101_2 = " "; long long tb_c1101_2n = sizeof(" ") - 1; const char *tb_c1101_3 = f_emit__fname(v_f); long long tb_c1101_3n = (long long)strlen(tb_c1101_3); const char *tb_c1101_4 = "("; long long tb_c1101_4n = sizeof("(") - 1; char *tb_c1101r = tb_mem(tb_c1101_0n + tb_c1101_1n + tb_c1101_2n + tb_c1101_3n + tb_c1101_4n + 1), *tb_c1101rw = tb_c1101r; memcpy(tb_c1101rw, tb_c1101_0, tb_c1101_0n); tb_c1101rw += tb_c1101_0n; memcpy(tb_c1101rw, tb_c1101_1, tb_c1101_1n); tb_c1101rw += tb_c1101_1n; memcpy(tb_c1101rw, tb_c1101_2, tb_c1101_2n); tb_c1101rw += tb_c1101_2n; memcpy(tb_c1101rw, tb_c1101_3, tb_c1101_3n); tb_c1101rw += tb_c1101_3n; memcpy(tb_c1101rw, tb_c1101_4, tb_c1101_4n); tb_c1101rw += tb_c1101_4n; *tb_c1101rw = 0; (const char *)tb_c1101r; }));
    if (((((tb_o_Node *)tb_nn(v_params, 1419))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end1102 = ((((tb_o_Node *)tb_nn(v_params, 1421))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1102; v_i++) {
            tb_o_Node * v_p = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1422))->q_kids, v_i, sizeof(tb_o_Node *), 1422));
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            if (((tb_o_Node *)tb_nn(v_p, 1425))->q_is_link) {
                f_emit__put(v_c, ({ const char *tb_c1103_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1426))->q_ty); long long tb_c1103_0n = (long long)strlen(tb_c1103_0); const char *tb_c1103_1 = " *v_"; long long tb_c1103_1n = sizeof(" *v_") - 1; const char *tb_c1103_2 = ((tb_o_Node *)tb_nn(v_p, 1426))->q_name; long long tb_c1103_2n = (long long)strlen(tb_c1103_2); char *tb_c1103r = tb_mem(tb_c1103_0n + tb_c1103_1n + tb_c1103_2n + 1), *tb_c1103rw = tb_c1103r; memcpy(tb_c1103rw, tb_c1103_0, tb_c1103_0n); tb_c1103rw += tb_c1103_0n; memcpy(tb_c1103rw, tb_c1103_1, tb_c1103_1n); tb_c1103rw += tb_c1103_1n; memcpy(tb_c1103rw, tb_c1103_2, tb_c1103_2n); tb_c1103rw += tb_c1103_2n; *tb_c1103rw = 0; (const char *)tb_c1103r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c1104_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1428))->q_ty); long long tb_c1104_0n = (long long)strlen(tb_c1104_0); const char *tb_c1104_1 = " v_"; long long tb_c1104_1n = sizeof(" v_") - 1; const char *tb_c1104_2 = ((tb_o_Node *)tb_nn(v_p, 1428))->q_name; long long tb_c1104_2n = (long long)strlen(tb_c1104_2); char *tb_c1104r = tb_mem(tb_c1104_0n + tb_c1104_1n + tb_c1104_2n + 1), *tb_c1104rw = tb_c1104r; memcpy(tb_c1104rw, tb_c1104_0, tb_c1104_0n); tb_c1104rw += tb_c1104_0n; memcpy(tb_c1104rw, tb_c1104_1, tb_c1104_1n); tb_c1104rw += tb_c1104_1n; memcpy(tb_c1104rw, tb_c1104_2, tb_c1104_2n); tb_c1104rw += tb_c1104_2n; *tb_c1104rw = 0; (const char *)tb_c1104r; }));
            }
        }
    }
    f_emit__put(v_c, ")");
}

static _Bool f_emit__bld_has(tb_list * v_names, const char * v_name) {
    {
        tb_list * tb_l1105 = v_names;
        for (long long tb_i1105 = 0; tb_i1105 < tb_l1105->len; tb_i1105++) {
            const char * const v_x = ((const char * *)tb_l1105->data)[tb_i1105];
            if ((strcmp(v_x, v_name) == 0)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1438, "функция bld_has дошла до конца без return");
    return 0;
}

static _Bool f_emit__bld_uses(tb_o_Node * v_n, const char * v_name) {
    if ((({ const char *tb_s1106 = ((tb_o_Node *)tb_nn(v_n, 1445))->q_kind; ((unsigned char)tb_s1106[0] == 110 && (unsigned char)tb_s1106[1] == 97 && (unsigned char)tb_s1106[2] == 109 && (unsigned char)tb_s1106[3] == 101 && tb_s1106[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1445))->q_name, v_name) == 0))) {
        return 1;
    }
    {
        tb_list * tb_l1107 = ((tb_o_Node *)tb_nn(v_n, 1447))->q_kids;
        for (long long tb_i1107 = 0; tb_i1107 < tb_l1107->len; tb_i1107++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l1107->data)[tb_i1107];
            if (f_emit__bld_uses(v_k, v_name)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1444, "функция bld_uses дошла до конца без return");
    return 0;
}

static void f_emit__bld_bad_all(tb_o_Node * v_n, tb_list * v_bad) {
    if ((({ const char *tb_s1108 = ((tb_o_Node *)tb_nn(v_n, 1454))->q_kind; ((unsigned char)tb_s1108[0] == 110 && (unsigned char)tb_s1108[1] == 97 && (unsigned char)tb_s1108[2] == 109 && (unsigned char)tb_s1108[3] == 101 && tb_s1108[4] == 0); }) || ({ const char *tb_s1109 = ((tb_o_Node *)tb_nn(v_n, 1454))->q_kind; ((unsigned char)tb_s1109[0] == 97 && (unsigned char)tb_s1109[1] == 115 && (unsigned char)tb_s1109[2] == 115 && (unsigned char)tb_s1109[3] == 105 && (unsigned char)tb_s1109[4] == 103 && (unsigned char)tb_s1109[5] == 110 && tb_s1109[6] == 0); }))) {
        { const char * tb_v1110 = ((tb_o_Node *)tb_nn(v_n, 1455))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1110; }
    }
    {
        tb_list * tb_l1111 = ((tb_o_Node *)tb_nn(v_n, 1456))->q_kids;
        for (long long tb_i1111 = 0; tb_i1111 < tb_l1111->len; tb_i1111++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l1111->data)[tb_i1111];
            f_emit__bld_bad_all(v_k, v_bad);
        }
    }
}

static _Bool f_emit__is_text_plus(tb_o_Comp * v_c, tb_o_Node * v_n) {
    return ((({ const char *tb_s1112 = ((tb_o_Node *)tb_nn(v_n, 1460))->q_kind; ((unsigned char)tb_s1112[0] == 98 && (unsigned char)tb_s1112[1] == 105 && (unsigned char)tb_s1112[2] == 110 && (unsigned char)tb_s1112[3] == 111 && (unsigned char)tb_s1112[4] == 112 && tb_s1112[5] == 0); }) && ({ const char *tb_s1113 = ((tb_o_Node *)tb_nn(v_n, 1460))->q_value; ((unsigned char)tb_s1113[0] == 43 && tb_s1113[1] == 0); })) && ({ const char *tb_s1114 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 1460))->q_ty); ((unsigned char)tb_s1114[0] == 116 && (unsigned char)tb_s1114[1] == 101 && (unsigned char)tb_s1114[2] == 120 && (unsigned char)tb_s1114[3] == 116 && tb_s1114[4] == 0); }));
    tb_crash(1459, "функция is_text_plus дошла до конца без return");
    return 0;
}

static _Bool f_emit__app_form(tb_o_Comp * v_c, const char * v_name, tb_o_Node * v_v) {
    if ((!f_emit__is_text_plus(v_c, v_v))) {
        return 0;
    }
    tb_o_Node * v_r = v_v;
    while (f_emit__is_text_plus(v_c, v_r)) {
        if (f_emit__bld_uses((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1468))->q_kids, 1LL, sizeof(tb_o_Node *), 1468)), v_name)) {
            return 0;
        }
        v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1470))->q_kids, 0LL, sizeof(tb_o_Node *), 1470));
    }
    return ((({ const char *tb_s1115 = ((tb_o_Node *)tb_nn(v_r, 1471))->q_kind; ((unsigned char)tb_s1115[0] == 110 && (unsigned char)tb_s1115[1] == 97 && (unsigned char)tb_s1115[2] == 109 && (unsigned char)tb_s1115[3] == 101 && tb_s1115[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_r, 1471))->q_name, v_name) == 0)) && (!((tb_o_Node *)tb_nn(v_r, 1471))->q_is_link));
    tb_crash(1463, "функция app_form дошла до конца без return");
    return 0;
}

static tb_list * f_emit__app_parts(tb_o_Comp * v_c, tb_o_Node * v_v) {
    tb_list * v_parts = tb_list_new(sizeof(tb_o_Node *));
    tb_list * v_back = tb_list_new(sizeof(tb_o_Node *));
    tb_o_Node * v_r = v_v;
    while (f_emit__is_text_plus(v_c, v_r)) {
        { tb_o_Node * tb_v1116 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1478))->q_kids, 1LL, sizeof(tb_o_Node *), 1478)); *(tb_o_Node * *)tb_grow(v_back) = tb_v1116; }
        v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1479))->q_kids, 0LL, sizeof(tb_o_Node *), 1479));
    }
    long long v_i = ((v_back)->len - 1LL);
    while ((v_i >= 0LL)) {
        { tb_o_Node * tb_v1117 = (*(tb_o_Node * *)tb_at(v_back, v_i, sizeof(tb_o_Node *), 1482)); *(tb_o_Node * *)tb_grow(v_parts) = tb_v1117; }
        { long long tb_v1118 = 1LL; long long *tb_p1118 = &(v_i); *tb_p1118 -= tb_v1118; }
    }
    return v_parts;
}

static void f_emit__bld_scan(tb_o_Comp * v_c, tb_o_Node * v_n, _Bool v_in_par, tb_list * v_cand, tb_list * v_bad) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1486))->q_kind;
    _Bool v_par = (((v_in_par || (strcmp(v_k, "background") == 0)) || ({ const char *tb_s1119 = v_k; ((unsigned char)tb_s1119[0] == 116 && (unsigned char)tb_s1119[1] == 111 && (unsigned char)tb_s1119[2] == 103 && (unsigned char)tb_s1119[3] == 101 && (unsigned char)tb_s1119[4] == 116 && (unsigned char)tb_s1119[5] == 104 && (unsigned char)tb_s1119[6] == 101 && (unsigned char)tb_s1119[7] == 114 && tb_s1119[8] == 0); })) || ((({ const char *tb_s1120 = v_k; ((unsigned char)tb_s1120[0] == 101 && (unsigned char)tb_s1120[1] == 97 && (unsigned char)tb_s1120[2] == 99 && (unsigned char)tb_s1120[3] == 104 && tb_s1120[4] == 0); }) || ({ const char *tb_s1121 = v_k; ((unsigned char)tb_s1121[0] == 99 && (unsigned char)tb_s1121[1] == 111 && (unsigned char)tb_s1121[2] == 117 && (unsigned char)tb_s1121[3] == 110 && (unsigned char)tb_s1121[4] == 116 && tb_s1121[5] == 0); })) && (((tb_o_Node *)tb_nn(v_n, 1487))->q_is_par || ((tb_o_Node *)tb_nn(v_n, 1487))->q_by_vec)));
    if (v_par) {
        f_emit__bld_bad_all(v_n, v_bad);
        return;
    }
    if ((({ const char *tb_s1122 = v_k; ((unsigned char)tb_s1122[0] == 108 && (unsigned char)tb_s1122[1] == 105 && (unsigned char)tb_s1122[2] == 110 && (unsigned char)tb_s1122[3] == 107 && (unsigned char)tb_s1122[4] == 100 && (unsigned char)tb_s1122[5] == 101 && (unsigned char)tb_s1122[6] == 99 && (unsigned char)tb_s1122[7] == 108 && tb_s1122[8] == 0); }) || ({ const char *tb_s1123 = v_k; ((unsigned char)tb_s1123[0] == 109 && (unsigned char)tb_s1123[1] == 117 && (unsigned char)tb_s1123[2] == 108 && (unsigned char)tb_s1123[3] == 116 && (unsigned char)tb_s1123[4] == 105 && tb_s1123[5] == 0); }))) {
        f_emit__bld_bad_all(v_n, v_bad);
    }
    if (((({ const char *tb_s1124 = v_k; ((unsigned char)tb_s1124[0] == 101 && (unsigned char)tb_s1124[1] == 97 && (unsigned char)tb_s1124[2] == 99 && (unsigned char)tb_s1124[3] == 104 && tb_s1124[4] == 0); }) || ({ const char *tb_s1125 = v_k; ((unsigned char)tb_s1125[0] == 99 && (unsigned char)tb_s1125[1] == 111 && (unsigned char)tb_s1125[2] == 117 && (unsigned char)tb_s1125[3] == 110 && (unsigned char)tb_s1125[4] == 116 && tb_s1125[5] == 0); })) || ({ const char *tb_s1126 = v_k; ((unsigned char)tb_s1126[0] == 101 && (unsigned char)tb_s1126[1] == 108 && (unsigned char)tb_s1126[2] == 115 && (unsigned char)tb_s1126[3] == 101 && (unsigned char)tb_s1126[4] == 95 && (unsigned char)tb_s1126[5] == 101 && (unsigned char)tb_s1126[6] == 114 && (unsigned char)tb_s1126[7] == 114 && tb_s1126[8] == 0); }))) {
        { const char * tb_v1127 = ((tb_o_Node *)tb_nn(v_n, 1494))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1127; }
    }
    if (((({ const char *tb_s1128 = v_k; ((unsigned char)tb_s1128[0] == 99 && (unsigned char)tb_s1128[1] == 97 && (unsigned char)tb_s1128[2] == 108 && (unsigned char)tb_s1128[3] == 108 && tb_s1128[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1495))->q_has_ref) && ({ const char *tb_s1129 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1495))->q_ref, 1495))->q_kind; ((unsigned char)tb_s1129[0] == 102 && (unsigned char)tb_s1129[1] == 117 && (unsigned char)tb_s1129[2] == 110 && (unsigned char)tb_s1129[3] == 99 && tb_s1129[4] == 0); }))) {
        tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1496))->q_ref, 1496))->q_kids, 0LL, sizeof(tb_o_Node *), 1496));
        {
            long long tb_end1130 = ((((tb_o_Node *)tb_nn(v_n, 1497))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end1130; v_i++) {
                if (((v_i < (((tb_o_Node *)tb_nn(v_params, 1498))->q_kids)->len) && ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1498))->q_kids, v_i, sizeof(tb_o_Node *), 1498)), 1498))->q_is_link)) {
                    f_emit__bld_bad_all((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1499))->q_kids, v_i, sizeof(tb_o_Node *), 1499)), v_bad);
                }
            }
        }
    }
    if (({ const char *tb_s1131 = v_k; ((unsigned char)tb_s1131[0] == 97 && (unsigned char)tb_s1131[1] == 115 && (unsigned char)tb_s1131[2] == 115 && (unsigned char)tb_s1131[3] == 105 && (unsigned char)tb_s1131[4] == 103 && (unsigned char)tb_s1131[5] == 110 && tb_s1131[6] == 0); })) {
        if (((tb_o_Node *)tb_nn(v_n, 1501))->q_is_link) {
            { const char * tb_v1132 = ((tb_o_Node *)tb_nn(v_n, 1502))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1132; }
        } else if ((((({ const char *tb_s1133 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 1503))->q_ty); ((unsigned char)tb_s1133[0] == 116 && (unsigned char)tb_s1133[1] == 101 && (unsigned char)tb_s1133[2] == 120 && (unsigned char)tb_s1133[3] == 116 && tb_s1133[4] == 0); }) && ((((tb_o_Node *)tb_nn(v_n, 1503))->q_kids)->len == 1LL)) && (!((tb_o_Node *)tb_nn(v_n, 1503))->q_creates)) && f_emit__app_form(v_c, ((tb_o_Node *)tb_nn(v_n, 1503))->q_name, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1503))->q_kids, 0LL, sizeof(tb_o_Node *), 1503))))) {
            { const char * tb_v1134 = ((tb_o_Node *)tb_nn(v_n, 1504))->q_name; *(const char * *)tb_grow(v_cand) = tb_v1134; }
        }
    }
    if ((((({ const char *tb_s1135 = v_k; ((unsigned char)tb_s1135[0] == 115 && (unsigned char)tb_s1135[1] == 101 && (unsigned char)tb_s1135[2] == 116 && tb_s1135[3] == 0); }) && ({ const char *tb_s1136 = ((tb_o_Node *)tb_nn(v_n, 1505))->q_value; ((unsigned char)tb_s1136[0] == 43 && (unsigned char)tb_s1136[1] == 61 && tb_s1136[2] == 0); })) && ({ const char *tb_s1137 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1505))->q_kids, 0LL, sizeof(tb_o_Node *), 1505)), 1505))->q_kind; ((unsigned char)tb_s1137[0] == 110 && (unsigned char)tb_s1137[1] == 97 && (unsigned char)tb_s1137[2] == 109 && (unsigned char)tb_s1137[3] == 101 && tb_s1137[4] == 0); })) && ({ const char *tb_s1138 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1505))->q_kids, 0LL, sizeof(tb_o_Node *), 1505)), 1505))->q_ty); ((unsigned char)tb_s1138[0] == 116 && (unsigned char)tb_s1138[1] == 101 && (unsigned char)tb_s1138[2] == 120 && (unsigned char)tb_s1138[3] == 116 && tb_s1138[4] == 0); }))) {
        if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1506))->q_kids, 0LL, sizeof(tb_o_Node *), 1506)), 1506))->q_is_link) {
            { const char * tb_v1139 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1507))->q_kids, 0LL, sizeof(tb_o_Node *), 1507)), 1507))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1139; }
        } else {
            { const char * tb_v1140 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1509))->q_kids, 0LL, sizeof(tb_o_Node *), 1509)), 1509))->q_name; *(const char * *)tb_grow(v_cand) = tb_v1140; }
        }
    }
    {
        tb_list * tb_l1141 = ((tb_o_Node *)tb_nn(v_n, 1510))->q_kids;
        for (long long tb_i1141 = 0; tb_i1141 < tb_l1141->len; tb_i1141++) {
            tb_o_Node * const v_kid = ((tb_o_Node * *)tb_l1141->data)[tb_i1141];
            f_emit__bld_scan(v_c, v_kid, 0, v_cand, v_bad);
        }
    }
}

static void f_emit__find_builders(tb_o_Comp * v_c, tb_o_Node * v_f) {
    tb_list * v_fresh = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1142 = v_fresh; tb_list * *tb_p1142 = &(((tb_o_Comp *)tb_nn(v_c, 1515))->q_builders); *tb_p1142 = tb_v1142; }
    tb_list * v_cand = tb_list_new(sizeof(const char *));
    tb_list * v_bad = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l1143 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1518))->q_kids, 0LL, sizeof(tb_o_Node *), 1518)), 1518))->q_kids;
        for (long long tb_i1143 = 0; tb_i1143 < tb_l1143->len; tb_i1143++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l1143->data)[tb_i1143];
            if (((tb_o_Node *)tb_nn(v_p, 1519))->q_is_link) {
                { const char * tb_v1144 = ((tb_o_Node *)tb_nn(v_p, 1520))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1144; }
            }
        }
    }
    f_emit__bld_scan(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1521))->q_kids, 1LL, sizeof(tb_o_Node *), 1521)), 0, v_cand, v_bad);
    {
        tb_list * tb_l1145 = v_cand;
        for (long long tb_i1145 = 0; tb_i1145 < tb_l1145->len; tb_i1145++) {
            const char * const v_name = ((const char * *)tb_l1145->data)[tb_i1145];
            if (((!f_emit__bld_has(v_bad, v_name)) && (!f_emit__bld_has(((tb_o_Comp *)tb_nn(v_c, 1523))->q_builders, v_name)))) {
                { const char * tb_v1146 = v_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1524))->q_builders) = tb_v1146; }
            }
        }
    }
}

static _Bool f_emit__is_bld(tb_o_Comp * v_c, tb_o_Node * v_n) {
    return ((((({ const char *tb_s1147 = ((tb_o_Node *)tb_nn(v_n, 1528))->q_kind; ((unsigned char)tb_s1147[0] == 110 && (unsigned char)tb_s1147[1] == 97 && (unsigned char)tb_s1147[2] == 109 && (unsigned char)tb_s1147[3] == 101 && tb_s1147[4] == 0); }) && (!((tb_o_Node *)tb_nn(v_n, 1528))->q_is_link)) && ((((tb_o_Comp *)tb_nn(v_c, 1528))->q_builders)->len > 0LL)) && ({ const char *tb_s1148 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 1528))->q_ty); ((unsigned char)tb_s1148[0] == 116 && (unsigned char)tb_s1148[1] == 101 && (unsigned char)tb_s1148[2] == 120 && (unsigned char)tb_s1148[3] == 116 && tb_s1148[4] == 0); })) && f_emit__bld_has(((tb_o_Comp *)tb_nn(v_c, 1528))->q_builders, ((tb_o_Node *)tb_nn(v_n, 1528))->q_name));
    tb_crash(1527, "функция is_bld дошла до конца без return");
    return 0;
}

static void f_emit__emit_q(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if (f_emit__is_bld(v_c, v_n)) {
        f_emit__put(v_c, ({ const char *tb_c1149_0 = "v_"; long long tb_c1149_0n = sizeof("v_") - 1; const char *tb_c1149_1 = ((tb_o_Node *)tb_nn(v_n, 1533))->q_name; long long tb_c1149_1n = (long long)strlen(tb_c1149_1); char *tb_c1149r = tb_mem(tb_c1149_0n + tb_c1149_1n + 1), *tb_c1149rw = tb_c1149r; memcpy(tb_c1149rw, tb_c1149_0, tb_c1149_0n); tb_c1149rw += tb_c1149_0n; memcpy(tb_c1149rw, tb_c1149_1, tb_c1149_1n); tb_c1149rw += tb_c1149_1n; *tb_c1149rw = 0; (const char *)tb_c1149r; }));
    } else {
        f_emit__emit_expr(v_c, v_n);
    }
}

static void f_emit__emit_app(tb_o_Comp * v_c, const char * v_name, tb_o_Node * v_part) {
    const char * v_vars = ({ const char *tb_c1150_0 = "&v_"; long long tb_c1150_0n = sizeof("&v_") - 1; const char *tb_c1150_1 = v_name; long long tb_c1150_1n = (long long)strlen(tb_c1150_1); const char *tb_c1150_2 = ", &tb_rl_"; long long tb_c1150_2n = sizeof(", &tb_rl_") - 1; const char *tb_c1150_3 = v_name; long long tb_c1150_3n = (long long)strlen(tb_c1150_3); const char *tb_c1150_4 = ", &tb_rc_"; long long tb_c1150_4n = sizeof(", &tb_rc_") - 1; const char *tb_c1150_5 = v_name; long long tb_c1150_5n = (long long)strlen(tb_c1150_5); const char *tb_c1150_6 = ", "; long long tb_c1150_6n = sizeof(", ") - 1; char *tb_c1150r = tb_mem(tb_c1150_0n + tb_c1150_1n + tb_c1150_2n + tb_c1150_3n + tb_c1150_4n + tb_c1150_5n + tb_c1150_6n + 1), *tb_c1150rw = tb_c1150r; memcpy(tb_c1150rw, tb_c1150_0, tb_c1150_0n); tb_c1150rw += tb_c1150_0n; memcpy(tb_c1150rw, tb_c1150_1, tb_c1150_1n); tb_c1150rw += tb_c1150_1n; memcpy(tb_c1150rw, tb_c1150_2, tb_c1150_2n); tb_c1150rw += tb_c1150_2n; memcpy(tb_c1150rw, tb_c1150_3, tb_c1150_3n); tb_c1150rw += tb_c1150_3n; memcpy(tb_c1150rw, tb_c1150_4, tb_c1150_4n); tb_c1150rw += tb_c1150_4n; memcpy(tb_c1150rw, tb_c1150_5, tb_c1150_5n); tb_c1150rw += tb_c1150_5n; memcpy(tb_c1150rw, tb_c1150_6, tb_c1150_6n); tb_c1150rw += tb_c1150_6n; *tb_c1150rw = 0; (const char *)tb_c1150r; });
    if (({ const char *tb_s1151 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_part, 1539))->q_ty); ((unsigned char)tb_s1151[0] == 105 && (unsigned char)tb_s1151[1] == 110 && (unsigned char)tb_s1151[2] == 116 && tb_s1151[3] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c1152_0 = "tb_app_i("; long long tb_c1152_0n = sizeof("tb_app_i(") - 1; const char *tb_c1152_1 = v_vars; long long tb_c1152_1n = (long long)strlen(tb_c1152_1); char *tb_c1152r = tb_mem(tb_c1152_0n + tb_c1152_1n + 1), *tb_c1152rw = tb_c1152r; memcpy(tb_c1152rw, tb_c1152_0, tb_c1152_0n); tb_c1152rw += tb_c1152_0n; memcpy(tb_c1152rw, tb_c1152_1, tb_c1152_1n); tb_c1152rw += tb_c1152_1n; *tb_c1152rw = 0; (const char *)tb_c1152r; }));
        f_emit__emit_expr(v_c, v_part);
        f_emit__put(v_c, ");");
    } else if (({ const char *tb_s1153 = ((tb_o_Node *)tb_nn(v_part, 1544))->q_kind; ((unsigned char)tb_s1153[0] == 115 && (unsigned char)tb_s1153[1] == 116 && (unsigned char)tb_s1153[2] == 114 && (unsigned char)tb_s1153[3] == 105 && (unsigned char)tb_s1153[4] == 110 && (unsigned char)tb_s1153[5] == 103 && tb_s1153[6] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c1154_0 = "tb_app_n("; long long tb_c1154_0n = sizeof("tb_app_n(") - 1; const char *tb_c1154_1 = v_vars; long long tb_c1154_1n = (long long)strlen(tb_c1154_1); const char *tb_c1154_2 = ((tb_o_Node *)tb_nn(v_part, 1546))->q_value; long long tb_c1154_2n = (long long)strlen(tb_c1154_2); const char *tb_c1154_3 = ", sizeof("; long long tb_c1154_3n = sizeof(", sizeof(") - 1; const char *tb_c1154_4 = ((tb_o_Node *)tb_nn(v_part, 1546))->q_value; long long tb_c1154_4n = (long long)strlen(tb_c1154_4); const char *tb_c1154_5 = ") - 1);"; long long tb_c1154_5n = sizeof(") - 1);") - 1; char *tb_c1154r = tb_mem(tb_c1154_0n + tb_c1154_1n + tb_c1154_2n + tb_c1154_3n + tb_c1154_4n + tb_c1154_5n + 1), *tb_c1154rw = tb_c1154r; memcpy(tb_c1154rw, tb_c1154_0, tb_c1154_0n); tb_c1154rw += tb_c1154_0n; memcpy(tb_c1154rw, tb_c1154_1, tb_c1154_1n); tb_c1154rw += tb_c1154_1n; memcpy(tb_c1154rw, tb_c1154_2, tb_c1154_2n); tb_c1154rw += tb_c1154_2n; memcpy(tb_c1154rw, tb_c1154_3, tb_c1154_3n); tb_c1154rw += tb_c1154_3n; memcpy(tb_c1154rw, tb_c1154_4, tb_c1154_4n); tb_c1154rw += tb_c1154_4n; memcpy(tb_c1154rw, tb_c1154_5, tb_c1154_5n); tb_c1154rw += tb_c1154_5n; *tb_c1154rw = 0; (const char *)tb_c1154r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c1155_0 = "tb_app("; long long tb_c1155_0n = sizeof("tb_app(") - 1; const char *tb_c1155_1 = v_vars; long long tb_c1155_1n = (long long)strlen(tb_c1155_1); char *tb_c1155r = tb_mem(tb_c1155_0n + tb_c1155_1n + 1), *tb_c1155rw = tb_c1155r; memcpy(tb_c1155rw, tb_c1155_0, tb_c1155_0n); tb_c1155rw += tb_c1155_0n; memcpy(tb_c1155rw, tb_c1155_1, tb_c1155_1n); tb_c1155rw += tb_c1155_1n; *tb_c1155rw = 0; (const char *)tb_c1155r; }));
        f_emit__emit_as_text(v_c, v_part);
        f_emit__put(v_c, ");");
    }
}

static void f_emit__emit_func(tb_o_Comp * v_c, tb_o_Node * v_f) {
    { tb_o_Node * tb_v1156 = v_f; tb_o_Node * *tb_p1156 = &(((tb_o_Comp *)tb_nn(v_c, 1553))->q_cur_func); *tb_p1156 = tb_v1156; }
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1554))->q_kids, 2LL, sizeof(tb_o_Node *), 1554));
    f_emit__put(v_c, "\n");
    f_emit__emit_signature(v_c, v_f);
    f_emit__put(v_c, " {\n");
    {
        tb_list * tb_l1157 = ((tb_o_Node *)tb_nn(v_outputs, 1558))->q_kids;
        for (long long tb_i1157 = 0; tb_i1157 < tb_l1157->len; tb_i1157++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l1157->data)[tb_i1157];
            if ((!((tb_o_Node *)tb_nn(v_o, 1559))->q_is_const)) {
                f_emit__put(v_c, ({ const char *tb_c1158_0 = "    "; long long tb_c1158_0n = sizeof("    ") - 1; const char *tb_c1158_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_o, 1560))->q_ty); long long tb_c1158_1n = (long long)strlen(tb_c1158_1); const char *tb_c1158_2 = " v_"; long long tb_c1158_2n = sizeof(" v_") - 1; const char *tb_c1158_3 = ((tb_o_Node *)tb_nn(v_o, 1560))->q_name; long long tb_c1158_3n = (long long)strlen(tb_c1158_3); const char *tb_c1158_4 = " = "; long long tb_c1158_4n = sizeof(" = ") - 1; char *tb_c1158r = tb_mem(tb_c1158_0n + tb_c1158_1n + tb_c1158_2n + tb_c1158_3n + tb_c1158_4n + 1), *tb_c1158rw = tb_c1158r; memcpy(tb_c1158rw, tb_c1158_0, tb_c1158_0n); tb_c1158rw += tb_c1158_0n; memcpy(tb_c1158rw, tb_c1158_1, tb_c1158_1n); tb_c1158rw += tb_c1158_1n; memcpy(tb_c1158rw, tb_c1158_2, tb_c1158_2n); tb_c1158rw += tb_c1158_2n; memcpy(tb_c1158rw, tb_c1158_3, tb_c1158_3n); tb_c1158rw += tb_c1158_3n; memcpy(tb_c1158rw, tb_c1158_4, tb_c1158_4n); tb_c1158rw += tb_c1158_4n; *tb_c1158rw = 0; (const char *)tb_c1158r; }));
                f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_o, 1561))->q_ty);
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__find_builders(v_c, v_f);
    {
        tb_list * tb_l1159 = ((tb_o_Comp *)tb_nn(v_c, 1564))->q_builders;
        for (long long tb_i1159 = 0; tb_i1159 < tb_l1159->len; tb_i1159++) {
            const char * const v_b = ((const char * *)tb_l1159->data)[tb_i1159];
            f_emit__put(v_c, ({ const char *tb_c1160_0 = "    long long tb_rl_"; long long tb_c1160_0n = sizeof("    long long tb_rl_") - 1; const char *tb_c1160_1 = v_b; long long tb_c1160_1n = (long long)strlen(tb_c1160_1); const char *tb_c1160_2 = " = -1, tb_rc_"; long long tb_c1160_2n = sizeof(" = -1, tb_rc_") - 1; const char *tb_c1160_3 = v_b; long long tb_c1160_3n = (long long)strlen(tb_c1160_3); const char *tb_c1160_4 = " = 0; (void)tb_rl_"; long long tb_c1160_4n = sizeof(" = 0; (void)tb_rl_") - 1; const char *tb_c1160_5 = v_b; long long tb_c1160_5n = (long long)strlen(tb_c1160_5); const char *tb_c1160_6 = "; (void)tb_rc_"; long long tb_c1160_6n = sizeof("; (void)tb_rc_") - 1; const char *tb_c1160_7 = v_b; long long tb_c1160_7n = (long long)strlen(tb_c1160_7); const char *tb_c1160_8 = ";\n"; long long tb_c1160_8n = sizeof(";\n") - 1; char *tb_c1160r = tb_mem(tb_c1160_0n + tb_c1160_1n + tb_c1160_2n + tb_c1160_3n + tb_c1160_4n + tb_c1160_5n + tb_c1160_6n + tb_c1160_7n + tb_c1160_8n + 1), *tb_c1160rw = tb_c1160r; memcpy(tb_c1160rw, tb_c1160_0, tb_c1160_0n); tb_c1160rw += tb_c1160_0n; memcpy(tb_c1160rw, tb_c1160_1, tb_c1160_1n); tb_c1160rw += tb_c1160_1n; memcpy(tb_c1160rw, tb_c1160_2, tb_c1160_2n); tb_c1160rw += tb_c1160_2n; memcpy(tb_c1160rw, tb_c1160_3, tb_c1160_3n); tb_c1160rw += tb_c1160_3n; memcpy(tb_c1160rw, tb_c1160_4, tb_c1160_4n); tb_c1160rw += tb_c1160_4n; memcpy(tb_c1160rw, tb_c1160_5, tb_c1160_5n); tb_c1160rw += tb_c1160_5n; memcpy(tb_c1160rw, tb_c1160_6, tb_c1160_6n); tb_c1160rw += tb_c1160_6n; memcpy(tb_c1160rw, tb_c1160_7, tb_c1160_7n); tb_c1160rw += tb_c1160_7n; memcpy(tb_c1160rw, tb_c1160_8, tb_c1160_8n); tb_c1160rw += tb_c1160_8n; *tb_c1160rw = 0; (const char *)tb_c1160r; }));
        }
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1566))->q_kids, 1LL, sizeof(tb_o_Node *), 1566)), 1LL);
    tb_list * v_no_builders = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1161 = v_no_builders; tb_list * *tb_p1161 = &(((tb_o_Comp *)tb_nn(v_c, 1568))->q_builders); *tb_p1161 = tb_v1161; }
    if (((((tb_o_Node *)tb_nn(v_outputs, 1569))->q_kids)->len > 0LL)) {
        f_emit__emit_gives_return(v_c, 1LL);
    } else if (({ const char *tb_s1162 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 1571))->q_ty); !((unsigned char)tb_s1162[0] == 110 && (unsigned char)tb_s1162[1] == 111 && (unsigned char)tb_s1162[2] == 110 && (unsigned char)tb_s1162[3] == 101 && tb_s1162[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c1163_0 = "    tb_crash("; long long tb_c1163_0n = sizeof("    tb_crash(") - 1; char tb_c1163_1d[24]; long long tb_c1163_1n; const char *tb_c1163_1 = tb_digits(((tb_o_Node *)tb_nn(v_f, 1572))->q_line, tb_c1163_1d, &tb_c1163_1n); const char *tb_c1163_2 = ", \"функция "; long long tb_c1163_2n = sizeof(", \"функция ") - 1; const char *tb_c1163_3 = ((tb_o_Node *)tb_nn(v_f, 1572))->q_name; long long tb_c1163_3n = (long long)strlen(tb_c1163_3); const char *tb_c1163_4 = " дошла до конца без return\");\n"; long long tb_c1163_4n = sizeof(" дошла до конца без return\");\n") - 1; char *tb_c1163r = tb_mem(tb_c1163_0n + tb_c1163_1n + tb_c1163_2n + tb_c1163_3n + tb_c1163_4n + 1), *tb_c1163rw = tb_c1163r; memcpy(tb_c1163rw, tb_c1163_0, tb_c1163_0n); tb_c1163rw += tb_c1163_0n; memcpy(tb_c1163rw, tb_c1163_1, tb_c1163_1n); tb_c1163rw += tb_c1163_1n; memcpy(tb_c1163rw, tb_c1163_2, tb_c1163_2n); tb_c1163rw += tb_c1163_2n; memcpy(tb_c1163rw, tb_c1163_3, tb_c1163_3n); tb_c1163rw += tb_c1163_3n; memcpy(tb_c1163rw, tb_c1163_4, tb_c1163_4n); tb_c1163rw += tb_c1163_4n; *tb_c1163rw = 0; (const char *)tb_c1163r; }));
        f_emit__put(v_c, "    return ");
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_f, 1574))->q_ty);
        f_emit__put(v_c, ";\n");
    }
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_show(tb_o_Comp * v_c, tb_o_Type * v_t) {
    const char * v_k = ((tb_o_Type *)tb_nn(v_t, 1580))->q_kind;
    if (((({ const char *tb_s1164 = v_k; ((unsigned char)tb_s1164[0] == 110 && (unsigned char)tb_s1164[1] == 111 && (unsigned char)tb_s1164[2] == 110 && (unsigned char)tb_s1164[3] == 101 && tb_s1164[4] == 0); }) || ({ const char *tb_s1165 = v_k; ((unsigned char)tb_s1165[0] == 109 && (unsigned char)tb_s1165[1] == 117 && (unsigned char)tb_s1165[2] == 108 && (unsigned char)tb_s1165[3] == 116 && (unsigned char)tb_s1165[4] == 105 && tb_s1165[5] == 0); })) || ({ const char *tb_s1166 = v_k; ((unsigned char)tb_s1166[0] == 117 && (unsigned char)tb_s1166[1] == 110 && (unsigned char)tb_s1166[2] == 107 && (unsigned char)tb_s1166[3] == 110 && (unsigned char)tb_s1166[4] == 111 && (unsigned char)tb_s1166[5] == 119 && (unsigned char)tb_s1166[6] == 110 && tb_s1166[7] == 0); }))) {
        return;
    }
    if (({ const char *tb_s1167 = v_k; ((unsigned char)tb_s1167[0] == 116 && (unsigned char)tb_s1167[1] == 97 && (unsigned char)tb_s1167[2] == 115 && (unsigned char)tb_s1167[3] == 107 && tb_s1167[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c1168_0 = "static void tb_show_"; long long tb_c1168_0n = sizeof("static void tb_show_") - 1; char tb_c1168_1d[24]; long long tb_c1168_1n; const char *tb_c1168_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1584))->q_id, tb_c1168_1d, &tb_c1168_1n); const char *tb_c1168_2 = "(tb_task *x) { (void)x; tb_ps(\"<background>\"); }\n"; long long tb_c1168_2n = sizeof("(tb_task *x) { (void)x; tb_ps(\"<background>\"); }\n") - 1; char *tb_c1168r = tb_mem(tb_c1168_0n + tb_c1168_1n + tb_c1168_2n + 1), *tb_c1168rw = tb_c1168r; memcpy(tb_c1168rw, tb_c1168_0, tb_c1168_0n); tb_c1168rw += tb_c1168_0n; memcpy(tb_c1168rw, tb_c1168_1, tb_c1168_1n); tb_c1168rw += tb_c1168_1n; memcpy(tb_c1168rw, tb_c1168_2, tb_c1168_2n); tb_c1168rw += tb_c1168_2n; *tb_c1168rw = 0; (const char *)tb_c1168r; }));
        return;
    }
    f_emit__put(v_c, ({ const char *tb_c1169_0 = "static void tb_show_"; long long tb_c1169_0n = sizeof("static void tb_show_") - 1; char tb_c1169_1d[24]; long long tb_c1169_1n; const char *tb_c1169_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1586))->q_id, tb_c1169_1d, &tb_c1169_1n); const char *tb_c1169_2 = "("; long long tb_c1169_2n = sizeof("(") - 1; const char *tb_c1169_3 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1586))->q_id); long long tb_c1169_3n = (long long)strlen(tb_c1169_3); const char *tb_c1169_4 = " x) {\n"; long long tb_c1169_4n = sizeof(" x) {\n") - 1; char *tb_c1169r = tb_mem(tb_c1169_0n + tb_c1169_1n + tb_c1169_2n + tb_c1169_3n + tb_c1169_4n + 1), *tb_c1169rw = tb_c1169r; memcpy(tb_c1169rw, tb_c1169_0, tb_c1169_0n); tb_c1169rw += tb_c1169_0n; memcpy(tb_c1169rw, tb_c1169_1, tb_c1169_1n); tb_c1169rw += tb_c1169_1n; memcpy(tb_c1169rw, tb_c1169_2, tb_c1169_2n); tb_c1169rw += tb_c1169_2n; memcpy(tb_c1169rw, tb_c1169_3, tb_c1169_3n); tb_c1169rw += tb_c1169_3n; memcpy(tb_c1169rw, tb_c1169_4, tb_c1169_4n); tb_c1169rw += tb_c1169_4n; *tb_c1169rw = 0; (const char *)tb_c1169r; }));
    if (({ const char *tb_s1170 = v_k; ((unsigned char)tb_s1170[0] == 105 && (unsigned char)tb_s1170[1] == 110 && (unsigned char)tb_s1170[2] == 116 && tb_s1170[3] == 0); })) {
        f_emit__put(v_c, "    printf(\"%lld\", x);\n");
    } else if (({ const char *tb_s1171 = v_k; ((unsigned char)tb_s1171[0] == 100 && (unsigned char)tb_s1171[1] == 101 && (unsigned char)tb_s1171[2] == 99 && tb_s1171[3] == 0); })) {
        f_emit__put(v_c, "    printf(\"%g\", x);\n");
    } else if (({ const char *tb_s1172 = v_k; ((unsigned char)tb_s1172[0] == 98 && (unsigned char)tb_s1172[1] == 111 && (unsigned char)tb_s1172[2] == 111 && (unsigned char)tb_s1172[3] == 108 && tb_s1172[4] == 0); })) {
        f_emit__put(v_c, "    tb_ps(x ? \"true\" : \"false\");\n");
    } else if (({ const char *tb_s1173 = v_k; ((unsigned char)tb_s1173[0] == 116 && (unsigned char)tb_s1173[1] == 101 && (unsigned char)tb_s1173[2] == 120 && (unsigned char)tb_s1173[3] == 116 && tb_s1173[4] == 0); })) {
        f_emit__put(v_c, "    tb_ps(x);\n");
    } else if (({ const char *tb_s1174 = v_k; ((unsigned char)tb_s1174[0] == 108 && (unsigned char)tb_s1174[1] == 105 && (unsigned char)tb_s1174[2] == 115 && (unsigned char)tb_s1174[3] == 116 && tb_s1174[4] == 0); })) {
        const char * v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1596))->q_elem);
        f_emit__put(v_c, "    tb_ps(\"[\");\n");
        f_emit__put(v_c, "    for (long long i = 0; i < x->len; i++) {\n");
        f_emit__put(v_c, "        if (i > 0) tb_ps(\", \");\n");
        if (f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn(v_t, 1600))->q_elem)) {
            f_emit__put(v_c, ({ const char *tb_c1175_0 = "        tb_show_"; long long tb_c1175_0n = sizeof("        tb_show_") - 1; char tb_c1175_1d[24]; long long tb_c1175_1n; const char *tb_c1175_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1601))->q_elem, tb_c1175_1d, &tb_c1175_1n); const char *tb_c1175_2 = "(("; long long tb_c1175_2n = sizeof("((") - 1; const char *tb_c1175_3 = v_elem; long long tb_c1175_3n = (long long)strlen(tb_c1175_3); const char *tb_c1175_4 = "){x, i});\n"; long long tb_c1175_4n = sizeof("){x, i});\n") - 1; char *tb_c1175r = tb_mem(tb_c1175_0n + tb_c1175_1n + tb_c1175_2n + tb_c1175_3n + tb_c1175_4n + 1), *tb_c1175rw = tb_c1175r; memcpy(tb_c1175rw, tb_c1175_0, tb_c1175_0n); tb_c1175rw += tb_c1175_0n; memcpy(tb_c1175rw, tb_c1175_1, tb_c1175_1n); tb_c1175rw += tb_c1175_1n; memcpy(tb_c1175rw, tb_c1175_2, tb_c1175_2n); tb_c1175rw += tb_c1175_2n; memcpy(tb_c1175rw, tb_c1175_3, tb_c1175_3n); tb_c1175rw += tb_c1175_3n; memcpy(tb_c1175rw, tb_c1175_4, tb_c1175_4n); tb_c1175rw += tb_c1175_4n; *tb_c1175rw = 0; (const char *)tb_c1175r; }));
        } else if ((((tb_o_Type *)tb_nn(v_t, 1602))->q_elem == 3LL)) {
            f_emit__put(v_c, "        tb_show_3(TB_BIT(x, i));\n");
        } else {
            f_emit__put(v_c, ({ const char *tb_c1176_0 = "        tb_show_"; long long tb_c1176_0n = sizeof("        tb_show_") - 1; char tb_c1176_1d[24]; long long tb_c1176_1n; const char *tb_c1176_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1605))->q_elem, tb_c1176_1d, &tb_c1176_1n); const char *tb_c1176_2 = "((("; long long tb_c1176_2n = sizeof("(((") - 1; const char *tb_c1176_3 = v_elem; long long tb_c1176_3n = (long long)strlen(tb_c1176_3); const char *tb_c1176_4 = " *)x->data)[i]);\n"; long long tb_c1176_4n = sizeof(" *)x->data)[i]);\n") - 1; char *tb_c1176r = tb_mem(tb_c1176_0n + tb_c1176_1n + tb_c1176_2n + tb_c1176_3n + tb_c1176_4n + 1), *tb_c1176rw = tb_c1176r; memcpy(tb_c1176rw, tb_c1176_0, tb_c1176_0n); tb_c1176rw += tb_c1176_0n; memcpy(tb_c1176rw, tb_c1176_1, tb_c1176_1n); tb_c1176rw += tb_c1176_1n; memcpy(tb_c1176rw, tb_c1176_2, tb_c1176_2n); tb_c1176rw += tb_c1176_2n; memcpy(tb_c1176rw, tb_c1176_3, tb_c1176_3n); tb_c1176rw += tb_c1176_3n; memcpy(tb_c1176rw, tb_c1176_4, tb_c1176_4n); tb_c1176rw += tb_c1176_4n; *tb_c1176rw = 0; (const char *)tb_c1176r; }));
        }
        f_emit__put(v_c, "    }\n");
        f_emit__put(v_c, "    tb_ps(\"]\");\n");
    } else if (({ const char *tb_s1177 = v_k; ((unsigned char)tb_s1177[0] == 100 && (unsigned char)tb_s1177[1] == 105 && (unsigned char)tb_s1177[2] == 99 && (unsigned char)tb_s1177[3] == 116 && tb_s1177[4] == 0); })) {
        const char * v_kt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1609))->q_key);
        const char * v_vt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1610))->q_elem);
        f_emit__put(v_c, "    tb_ps(\"{\");\n");
        f_emit__put(v_c, "    for (long long i = 0; i < x->keys->len; i++) {\n");
        f_emit__put(v_c, "        if (i > 0) tb_ps(\", \");\n");
        f_emit__put(v_c, ({ const char *tb_c1178_0 = "        tb_show_"; long long tb_c1178_0n = sizeof("        tb_show_") - 1; char tb_c1178_1d[24]; long long tb_c1178_1n; const char *tb_c1178_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1614))->q_key, tb_c1178_1d, &tb_c1178_1n); const char *tb_c1178_2 = "((("; long long tb_c1178_2n = sizeof("(((") - 1; const char *tb_c1178_3 = v_kt; long long tb_c1178_3n = (long long)strlen(tb_c1178_3); const char *tb_c1178_4 = " *)x->keys->data)[i]);\n"; long long tb_c1178_4n = sizeof(" *)x->keys->data)[i]);\n") - 1; char *tb_c1178r = tb_mem(tb_c1178_0n + tb_c1178_1n + tb_c1178_2n + tb_c1178_3n + tb_c1178_4n + 1), *tb_c1178rw = tb_c1178r; memcpy(tb_c1178rw, tb_c1178_0, tb_c1178_0n); tb_c1178rw += tb_c1178_0n; memcpy(tb_c1178rw, tb_c1178_1, tb_c1178_1n); tb_c1178rw += tb_c1178_1n; memcpy(tb_c1178rw, tb_c1178_2, tb_c1178_2n); tb_c1178rw += tb_c1178_2n; memcpy(tb_c1178rw, tb_c1178_3, tb_c1178_3n); tb_c1178rw += tb_c1178_3n; memcpy(tb_c1178rw, tb_c1178_4, tb_c1178_4n); tb_c1178rw += tb_c1178_4n; *tb_c1178rw = 0; (const char *)tb_c1178r; }));
        f_emit__put(v_c, "        tb_ps(\": \");\n");
        f_emit__put(v_c, ({ const char *tb_c1179_0 = "        tb_show_"; long long tb_c1179_0n = sizeof("        tb_show_") - 1; char tb_c1179_1d[24]; long long tb_c1179_1n; const char *tb_c1179_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1616))->q_elem, tb_c1179_1d, &tb_c1179_1n); const char *tb_c1179_2 = "((("; long long tb_c1179_2n = sizeof("(((") - 1; const char *tb_c1179_3 = v_vt; long long tb_c1179_3n = (long long)strlen(tb_c1179_3); const char *tb_c1179_4 = " *)x->vals->data)[i]);\n"; long long tb_c1179_4n = sizeof(" *)x->vals->data)[i]);\n") - 1; char *tb_c1179r = tb_mem(tb_c1179_0n + tb_c1179_1n + tb_c1179_2n + tb_c1179_3n + tb_c1179_4n + 1), *tb_c1179rw = tb_c1179r; memcpy(tb_c1179rw, tb_c1179_0, tb_c1179_0n); tb_c1179rw += tb_c1179_0n; memcpy(tb_c1179rw, tb_c1179_1, tb_c1179_1n); tb_c1179rw += tb_c1179_1n; memcpy(tb_c1179rw, tb_c1179_2, tb_c1179_2n); tb_c1179rw += tb_c1179_2n; memcpy(tb_c1179rw, tb_c1179_3, tb_c1179_3n); tb_c1179rw += tb_c1179_3n; memcpy(tb_c1179rw, tb_c1179_4, tb_c1179_4n); tb_c1179rw += tb_c1179_4n; *tb_c1179rw = 0; (const char *)tb_c1179r; }));
        f_emit__put(v_c, "    }\n");
        f_emit__put(v_c, "    tb_ps(\"}\");\n");
    } else if (({ const char *tb_s1180 = v_k; ((unsigned char)tb_s1180[0] == 111 && (unsigned char)tb_s1180[1] == 98 && (unsigned char)tb_s1180[2] == 106 && tb_s1180[3] == 0); })) {
        tb_o_Node * v_o = ((tb_o_Type *)tb_nn(v_t, 1620))->q_obj_node;
        if (((tb_o_Node *)tb_nn(v_o, 1621))->q_by_cols) {
            f_emit__put(v_c, "    if (x.c == 0) { tb_ps(\"пусто\"); return; }\n");
        } else {
            f_emit__put(v_c, "    if (x == 0) { tb_ps(\"пусто\"); return; }\n");
        }
        f_emit__put(v_c, ({ const char *tb_c1181_0 = "    tb_ps(\""; long long tb_c1181_0n = sizeof("    tb_ps(\"") - 1; const char *tb_c1181_1 = ((tb_o_Node *)tb_nn(v_o, 1625))->q_name; long long tb_c1181_1n = (long long)strlen(tb_c1181_1); const char *tb_c1181_2 = "(\");\n"; long long tb_c1181_2n = sizeof("(\");\n") - 1; char *tb_c1181r = tb_mem(tb_c1181_0n + tb_c1181_1n + tb_c1181_2n + 1), *tb_c1181rw = tb_c1181r; memcpy(tb_c1181rw, tb_c1181_0, tb_c1181_0n); tb_c1181rw += tb_c1181_0n; memcpy(tb_c1181rw, tb_c1181_1, tb_c1181_1n); tb_c1181rw += tb_c1181_1n; memcpy(tb_c1181rw, tb_c1181_2, tb_c1181_2n); tb_c1181rw += tb_c1181_2n; *tb_c1181rw = 0; (const char *)tb_c1181r; }));
        {
            long long tb_end1182 = ((((tb_o_Node *)tb_nn(v_o, 1626))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end1182; v_i++) {
                const char * v_sep = "";
                if ((v_i > 0LL)) {
                    v_sep = ", ";
                }
                f_emit__put(v_c, ({ const char *tb_c1183_0 = "    tb_ps(\""; long long tb_c1183_0n = sizeof("    tb_ps(\"") - 1; const char *tb_c1183_1 = v_sep; long long tb_c1183_1n = (long long)strlen(tb_c1183_1); const char *tb_c1183_2 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1630))->q_kids, v_i, sizeof(tb_o_Node *), 1630)), 1630))->q_name; long long tb_c1183_2n = (long long)strlen(tb_c1183_2); const char *tb_c1183_3 = ": \");\n"; long long tb_c1183_3n = sizeof(": \");\n") - 1; char *tb_c1183r = tb_mem(tb_c1183_0n + tb_c1183_1n + tb_c1183_2n + tb_c1183_3n + 1), *tb_c1183rw = tb_c1183r; memcpy(tb_c1183rw, tb_c1183_0, tb_c1183_0n); tb_c1183rw += tb_c1183_0n; memcpy(tb_c1183rw, tb_c1183_1, tb_c1183_1n); tb_c1183rw += tb_c1183_1n; memcpy(tb_c1183rw, tb_c1183_2, tb_c1183_2n); tb_c1183rw += tb_c1183_2n; memcpy(tb_c1183rw, tb_c1183_3, tb_c1183_3n); tb_c1183rw += tb_c1183_3n; *tb_c1183rw = 0; (const char *)tb_c1183r; }));
                if (((tb_o_Node *)tb_nn(v_o, 1631))->q_by_cols) {
                    f_emit__put(v_c, ({ const char *tb_c1184_0 = "    tb_show_"; long long tb_c1184_0n = sizeof("    tb_show_") - 1; char tb_c1184_1d[24]; long long tb_c1184_1n; const char *tb_c1184_1 = tb_digits(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1632))->q_kids, v_i, sizeof(tb_o_Node *), 1632)), 1632))->q_ty, tb_c1184_1d, &tb_c1184_1n); const char *tb_c1184_2 = "(x.c->q_"; long long tb_c1184_2n = sizeof("(x.c->q_") - 1; const char *tb_c1184_3 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1632))->q_kids, v_i, sizeof(tb_o_Node *), 1632)), 1632))->q_name; long long tb_c1184_3n = (long long)strlen(tb_c1184_3); const char *tb_c1184_4 = "[x.i]);\n"; long long tb_c1184_4n = sizeof("[x.i]);\n") - 1; char *tb_c1184r = tb_mem(tb_c1184_0n + tb_c1184_1n + tb_c1184_2n + tb_c1184_3n + tb_c1184_4n + 1), *tb_c1184rw = tb_c1184r; memcpy(tb_c1184rw, tb_c1184_0, tb_c1184_0n); tb_c1184rw += tb_c1184_0n; memcpy(tb_c1184rw, tb_c1184_1, tb_c1184_1n); tb_c1184rw += tb_c1184_1n; memcpy(tb_c1184rw, tb_c1184_2, tb_c1184_2n); tb_c1184rw += tb_c1184_2n; memcpy(tb_c1184rw, tb_c1184_3, tb_c1184_3n); tb_c1184rw += tb_c1184_3n; memcpy(tb_c1184rw, tb_c1184_4, tb_c1184_4n); tb_c1184rw += tb_c1184_4n; *tb_c1184rw = 0; (const char *)tb_c1184r; }));
                } else {
                    f_emit__put(v_c, ({ const char *tb_c1185_0 = "    tb_show_"; long long tb_c1185_0n = sizeof("    tb_show_") - 1; char tb_c1185_1d[24]; long long tb_c1185_1n; const char *tb_c1185_1 = tb_digits(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1634))->q_kids, v_i, sizeof(tb_o_Node *), 1634)), 1634))->q_ty, tb_c1185_1d, &tb_c1185_1n); const char *tb_c1185_2 = "(x"; long long tb_c1185_2n = sizeof("(x") - 1; const char *tb_c1185_3 = f_emit__field_path(v_o, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1634))->q_kids, v_i, sizeof(tb_o_Node *), 1634)), 1634))->q_name); long long tb_c1185_3n = (long long)strlen(tb_c1185_3); const char *tb_c1185_4 = ");\n"; long long tb_c1185_4n = sizeof(");\n") - 1; char *tb_c1185r = tb_mem(tb_c1185_0n + tb_c1185_1n + tb_c1185_2n + tb_c1185_3n + tb_c1185_4n + 1), *tb_c1185rw = tb_c1185r; memcpy(tb_c1185rw, tb_c1185_0, tb_c1185_0n); tb_c1185rw += tb_c1185_0n; memcpy(tb_c1185rw, tb_c1185_1, tb_c1185_1n); tb_c1185rw += tb_c1185_1n; memcpy(tb_c1185rw, tb_c1185_2, tb_c1185_2n); tb_c1185rw += tb_c1185_2n; memcpy(tb_c1185rw, tb_c1185_3, tb_c1185_3n); tb_c1185rw += tb_c1185_3n; memcpy(tb_c1185rw, tb_c1185_4, tb_c1185_4n); tb_c1185rw += tb_c1185_4n; *tb_c1185rw = 0; (const char *)tb_c1185r; }));
                }
            }
        }
        f_emit__put(v_c, "    tb_ps(\")\");\n");
    }
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_cols_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    f_emit__put(v_c, ({ const char *tb_c1186_0 = "struct tb_cols_"; long long tb_c1186_0n = sizeof("struct tb_cols_") - 1; const char *tb_c1186_1 = ((tb_o_Node *)tb_nn(v_o, 1642))->q_name; long long tb_c1186_1n = (long long)strlen(tb_c1186_1); const char *tb_c1186_2 = " {\n    long long len, cap;\n"; long long tb_c1186_2n = sizeof(" {\n    long long len, cap;\n") - 1; char *tb_c1186r = tb_mem(tb_c1186_0n + tb_c1186_1n + tb_c1186_2n + 1), *tb_c1186rw = tb_c1186r; memcpy(tb_c1186rw, tb_c1186_0, tb_c1186_0n); tb_c1186rw += tb_c1186_0n; memcpy(tb_c1186rw, tb_c1186_1, tb_c1186_1n); tb_c1186rw += tb_c1186_1n; memcpy(tb_c1186rw, tb_c1186_2, tb_c1186_2n); tb_c1186rw += tb_c1186_2n; *tb_c1186rw = 0; (const char *)tb_c1186r; }));
    {
        tb_list * tb_l1187 = ((tb_o_Node *)tb_nn(v_o, 1643))->q_kids;
        for (long long tb_i1187 = 0; tb_i1187 < tb_l1187->len; tb_i1187++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1187->data)[tb_i1187];
            f_emit__put(v_c, ({ const char *tb_c1188_0 = "    "; long long tb_c1188_0n = sizeof("    ") - 1; const char *tb_c1188_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1644))->q_ty); long long tb_c1188_1n = (long long)strlen(tb_c1188_1); const char *tb_c1188_2 = " *q_"; long long tb_c1188_2n = sizeof(" *q_") - 1; const char *tb_c1188_3 = ((tb_o_Node *)tb_nn(v_f, 1644))->q_name; long long tb_c1188_3n = (long long)strlen(tb_c1188_3); const char *tb_c1188_4 = ";\n"; long long tb_c1188_4n = sizeof(";\n") - 1; char *tb_c1188r = tb_mem(tb_c1188_0n + tb_c1188_1n + tb_c1188_2n + tb_c1188_3n + tb_c1188_4n + 1), *tb_c1188rw = tb_c1188r; memcpy(tb_c1188rw, tb_c1188_0, tb_c1188_0n); tb_c1188rw += tb_c1188_0n; memcpy(tb_c1188rw, tb_c1188_1, tb_c1188_1n); tb_c1188rw += tb_c1188_1n; memcpy(tb_c1188rw, tb_c1188_2, tb_c1188_2n); tb_c1188rw += tb_c1188_2n; memcpy(tb_c1188rw, tb_c1188_3, tb_c1188_3n); tb_c1188rw += tb_c1188_3n; memcpy(tb_c1188rw, tb_c1188_4, tb_c1188_4n); tb_c1188rw += tb_c1188_4n; *tb_c1188rw = 0; (const char *)tb_c1188r; }));
        }
    }
    f_emit__put(v_c, "};\n");
}

static void f_emit__emit_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((tb_o_Node *)tb_nn(v_o, 1648))->q_by_cols) {
        f_emit__emit_cols_obj(v_c, v_o);
        return;
    }
    if (f_emit__has_cold(v_o)) {
        f_emit__put(v_c, ({ const char *tb_c1189_0 = "struct tb_c_"; long long tb_c1189_0n = sizeof("struct tb_c_") - 1; const char *tb_c1189_1 = ((tb_o_Node *)tb_nn(v_o, 1653))->q_name; long long tb_c1189_1n = (long long)strlen(tb_c1189_1); const char *tb_c1189_2 = " {\n"; long long tb_c1189_2n = sizeof(" {\n") - 1; char *tb_c1189r = tb_mem(tb_c1189_0n + tb_c1189_1n + tb_c1189_2n + 1), *tb_c1189rw = tb_c1189r; memcpy(tb_c1189rw, tb_c1189_0, tb_c1189_0n); tb_c1189rw += tb_c1189_0n; memcpy(tb_c1189rw, tb_c1189_1, tb_c1189_1n); tb_c1189rw += tb_c1189_1n; memcpy(tb_c1189rw, tb_c1189_2, tb_c1189_2n); tb_c1189rw += tb_c1189_2n; *tb_c1189rw = 0; (const char *)tb_c1189r; }));
        {
            tb_list * tb_l1190 = ((tb_o_Node *)tb_nn(v_o, 1654))->q_kids;
            for (long long tb_i1190 = 0; tb_i1190 < tb_l1190->len; tb_i1190++) {
                tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1190->data)[tb_i1190];
                if (((tb_o_Node *)tb_nn(v_f, 1655))->q_is_cold) {
                    f_emit__put(v_c, ({ const char *tb_c1191_0 = "    "; long long tb_c1191_0n = sizeof("    ") - 1; const char *tb_c1191_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1656))->q_ty); long long tb_c1191_1n = (long long)strlen(tb_c1191_1); const char *tb_c1191_2 = " q_"; long long tb_c1191_2n = sizeof(" q_") - 1; const char *tb_c1191_3 = ((tb_o_Node *)tb_nn(v_f, 1656))->q_name; long long tb_c1191_3n = (long long)strlen(tb_c1191_3); const char *tb_c1191_4 = ";\n"; long long tb_c1191_4n = sizeof(";\n") - 1; char *tb_c1191r = tb_mem(tb_c1191_0n + tb_c1191_1n + tb_c1191_2n + tb_c1191_3n + tb_c1191_4n + 1), *tb_c1191rw = tb_c1191r; memcpy(tb_c1191rw, tb_c1191_0, tb_c1191_0n); tb_c1191rw += tb_c1191_0n; memcpy(tb_c1191rw, tb_c1191_1, tb_c1191_1n); tb_c1191rw += tb_c1191_1n; memcpy(tb_c1191rw, tb_c1191_2, tb_c1191_2n); tb_c1191rw += tb_c1191_2n; memcpy(tb_c1191rw, tb_c1191_3, tb_c1191_3n); tb_c1191rw += tb_c1191_3n; memcpy(tb_c1191rw, tb_c1191_4, tb_c1191_4n); tb_c1191rw += tb_c1191_4n; *tb_c1191rw = 0; (const char *)tb_c1191r; }));
                }
            }
        }
        f_emit__put(v_c, "};\n");
    }
    f_emit__put(v_c, ({ const char *tb_c1192_0 = "struct tb_o_"; long long tb_c1192_0n = sizeof("struct tb_o_") - 1; const char *tb_c1192_1 = ((tb_o_Node *)tb_nn(v_o, 1658))->q_name; long long tb_c1192_1n = (long long)strlen(tb_c1192_1); const char *tb_c1192_2 = " {\n"; long long tb_c1192_2n = sizeof(" {\n") - 1; char *tb_c1192r = tb_mem(tb_c1192_0n + tb_c1192_1n + tb_c1192_2n + 1), *tb_c1192rw = tb_c1192r; memcpy(tb_c1192rw, tb_c1192_0, tb_c1192_0n); tb_c1192rw += tb_c1192_0n; memcpy(tb_c1192rw, tb_c1192_1, tb_c1192_1n); tb_c1192rw += tb_c1192_1n; memcpy(tb_c1192rw, tb_c1192_2, tb_c1192_2n); tb_c1192rw += tb_c1192_2n; *tb_c1192rw = 0; (const char *)tb_c1192r; }));
    {
        tb_list * tb_l1193 = ((tb_o_Node *)tb_nn(v_o, 1659))->q_kids;
        for (long long tb_i1193 = 0; tb_i1193 < tb_l1193->len; tb_i1193++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1193->data)[tb_i1193];
            if ((!((tb_o_Node *)tb_nn(v_f, 1660))->q_is_cold)) {
                f_emit__put(v_c, ({ const char *tb_c1194_0 = "    "; long long tb_c1194_0n = sizeof("    ") - 1; const char *tb_c1194_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1661))->q_ty); long long tb_c1194_1n = (long long)strlen(tb_c1194_1); const char *tb_c1194_2 = " q_"; long long tb_c1194_2n = sizeof(" q_") - 1; const char *tb_c1194_3 = ((tb_o_Node *)tb_nn(v_f, 1661))->q_name; long long tb_c1194_3n = (long long)strlen(tb_c1194_3); const char *tb_c1194_4 = ";\n"; long long tb_c1194_4n = sizeof(";\n") - 1; char *tb_c1194r = tb_mem(tb_c1194_0n + tb_c1194_1n + tb_c1194_2n + tb_c1194_3n + tb_c1194_4n + 1), *tb_c1194rw = tb_c1194r; memcpy(tb_c1194rw, tb_c1194_0, tb_c1194_0n); tb_c1194rw += tb_c1194_0n; memcpy(tb_c1194rw, tb_c1194_1, tb_c1194_1n); tb_c1194rw += tb_c1194_1n; memcpy(tb_c1194rw, tb_c1194_2, tb_c1194_2n); tb_c1194rw += tb_c1194_2n; memcpy(tb_c1194rw, tb_c1194_3, tb_c1194_3n); tb_c1194rw += tb_c1194_3n; memcpy(tb_c1194rw, tb_c1194_4, tb_c1194_4n); tb_c1194rw += tb_c1194_4n; *tb_c1194rw = 0; (const char *)tb_c1194r; }));
            }
        }
    }
    if (f_emit__has_cold(v_o)) {
        f_emit__put(v_c, ({ const char *tb_c1195_0 = "    struct tb_c_"; long long tb_c1195_0n = sizeof("    struct tb_c_") - 1; const char *tb_c1195_1 = ((tb_o_Node *)tb_nn(v_o, 1663))->q_name; long long tb_c1195_1n = (long long)strlen(tb_c1195_1); const char *tb_c1195_2 = " *tb_cold;\n"; long long tb_c1195_2n = sizeof(" *tb_cold;\n") - 1; char *tb_c1195r = tb_mem(tb_c1195_0n + tb_c1195_1n + tb_c1195_2n + 1), *tb_c1195rw = tb_c1195r; memcpy(tb_c1195rw, tb_c1195_0, tb_c1195_0n); tb_c1195rw += tb_c1195_0n; memcpy(tb_c1195rw, tb_c1195_1, tb_c1195_1n); tb_c1195rw += tb_c1195_1n; memcpy(tb_c1195rw, tb_c1195_2, tb_c1195_2n); tb_c1195rw += tb_c1195_2n; *tb_c1195rw = 0; (const char *)tb_c1195r; }));
    }
    if (((((tb_o_Node *)tb_nn(v_o, 1664))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "    char empty;\n");
    }
    f_emit__put(v_c, "};\n");
}

static _Bool f_emit__has_cold(tb_o_Node * v_o) {
    {
        tb_list * tb_l1196 = ((tb_o_Node *)tb_nn(v_o, 1669))->q_kids;
        for (long long tb_i1196 = 0; tb_i1196 < tb_l1196->len; tb_i1196++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1196->data)[tb_i1196];
            if (((tb_o_Node *)tb_nn(v_f, 1670))->q_is_cold) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1668, "функция has_cold дошла до конца без return");
    return 0;
}

static const char * f_emit__field_path(tb_o_Node * v_o, const char * v_name) {
    const char * v_r = "";
    {
        tb_list * tb_l1197 = ((tb_o_Node *)tb_nn(v_o, 1676))->q_kids;
        for (long long tb_i1197 = 0; tb_i1197 < tb_l1197->len; tb_i1197++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1197->data)[tb_i1197];
            if (((strcmp(((tb_o_Node *)tb_nn(v_f, 1677))->q_name, v_name) == 0) && ((tb_o_Node *)tb_nn(v_f, 1677))->q_is_cold)) {
                return ({ const char *tb_c1198_0 = "->tb_cold->q_"; long long tb_c1198_0n = sizeof("->tb_cold->q_") - 1; const char *tb_c1198_1 = v_name; long long tb_c1198_1n = (long long)strlen(tb_c1198_1); char *tb_c1198r = tb_mem(tb_c1198_0n + tb_c1198_1n + 1), *tb_c1198rw = tb_c1198r; memcpy(tb_c1198rw, tb_c1198_0, tb_c1198_0n); tb_c1198rw += tb_c1198_0n; memcpy(tb_c1198rw, tb_c1198_1, tb_c1198_1n); tb_c1198rw += tb_c1198_1n; *tb_c1198rw = 0; (const char *)tb_c1198r; });
            }
        }
    }
    return ({ const char *tb_c1199_0 = "->q_"; long long tb_c1199_0n = sizeof("->q_") - 1; const char *tb_c1199_1 = v_name; long long tb_c1199_1n = (long long)strlen(tb_c1199_1); char *tb_c1199r = tb_mem(tb_c1199_0n + tb_c1199_1n + 1), *tb_c1199rw = tb_c1199r; memcpy(tb_c1199rw, tb_c1199_0, tb_c1199_0n); tb_c1199rw += tb_c1199_0n; memcpy(tb_c1199rw, tb_c1199_1, tb_c1199_1n); tb_c1199rw += tb_c1199_1n; *tb_c1199rw = 0; (const char *)tb_c1199r; });
    return v_r;
}

static void f_emit__emit_cols_helpers(tb_o_Comp * v_c, tb_o_Node * v_o) {
    const char * v_nm = ((tb_o_Node *)tb_nn(v_o, 1683))->q_name;
    const char * v_cols = ({ const char *tb_c1200_0 = "tb_cols_"; long long tb_c1200_0n = sizeof("tb_cols_") - 1; const char *tb_c1200_1 = v_nm; long long tb_c1200_1n = (long long)strlen(tb_c1200_1); char *tb_c1200r = tb_mem(tb_c1200_0n + tb_c1200_1n + 1), *tb_c1200rw = tb_c1200r; memcpy(tb_c1200rw, tb_c1200_0, tb_c1200_0n); tb_c1200rw += tb_c1200_0n; memcpy(tb_c1200rw, tb_c1200_1, tb_c1200_1n); tb_c1200rw += tb_c1200_1n; *tb_c1200rw = 0; (const char *)tb_c1200r; });
    const char * v_row = ({ const char *tb_c1201_0 = "tb_row_"; long long tb_c1201_0n = sizeof("tb_row_") - 1; const char *tb_c1201_1 = v_nm; long long tb_c1201_1n = (long long)strlen(tb_c1201_1); char *tb_c1201r = tb_mem(tb_c1201_0n + tb_c1201_1n + 1), *tb_c1201rw = tb_c1201r; memcpy(tb_c1201rw, tb_c1201_0, tb_c1201_0n); tb_c1201rw += tb_c1201_0n; memcpy(tb_c1201rw, tb_c1201_1, tb_c1201_1n); tb_c1201rw += tb_c1201_1n; *tb_c1201rw = 0; (const char *)tb_c1201r; });
    f_emit__put(v_c, ({ const char *tb_c1202_0 = "static "; long long tb_c1202_0n = sizeof("static ") - 1; const char *tb_c1202_1 = v_cols; long long tb_c1202_1n = (long long)strlen(tb_c1202_1); const char *tb_c1202_2 = " *tb_cols_new_"; long long tb_c1202_2n = sizeof(" *tb_cols_new_") - 1; const char *tb_c1202_3 = v_nm; long long tb_c1202_3n = (long long)strlen(tb_c1202_3); const char *tb_c1202_4 = "(void) {\n"; long long tb_c1202_4n = sizeof("(void) {\n") - 1; char *tb_c1202r = tb_mem(tb_c1202_0n + tb_c1202_1n + tb_c1202_2n + tb_c1202_3n + tb_c1202_4n + 1), *tb_c1202rw = tb_c1202r; memcpy(tb_c1202rw, tb_c1202_0, tb_c1202_0n); tb_c1202rw += tb_c1202_0n; memcpy(tb_c1202rw, tb_c1202_1, tb_c1202_1n); tb_c1202rw += tb_c1202_1n; memcpy(tb_c1202rw, tb_c1202_2, tb_c1202_2n); tb_c1202rw += tb_c1202_2n; memcpy(tb_c1202rw, tb_c1202_3, tb_c1202_3n); tb_c1202rw += tb_c1202_3n; memcpy(tb_c1202rw, tb_c1202_4, tb_c1202_4n); tb_c1202rw += tb_c1202_4n; *tb_c1202rw = 0; (const char *)tb_c1202r; }));
    f_emit__put(v_c, ({ const char *tb_c1203_0 = "    "; long long tb_c1203_0n = sizeof("    ") - 1; const char *tb_c1203_1 = v_cols; long long tb_c1203_1n = (long long)strlen(tb_c1203_1); const char *tb_c1203_2 = " *l = tb_mem(sizeof("; long long tb_c1203_2n = sizeof(" *l = tb_mem(sizeof(") - 1; const char *tb_c1203_3 = v_cols; long long tb_c1203_3n = (long long)strlen(tb_c1203_3); const char *tb_c1203_4 = "));\n    l->len = 0; l->cap = 4;\n"; long long tb_c1203_4n = sizeof("));\n    l->len = 0; l->cap = 4;\n") - 1; char *tb_c1203r = tb_mem(tb_c1203_0n + tb_c1203_1n + tb_c1203_2n + tb_c1203_3n + tb_c1203_4n + 1), *tb_c1203rw = tb_c1203r; memcpy(tb_c1203rw, tb_c1203_0, tb_c1203_0n); tb_c1203rw += tb_c1203_0n; memcpy(tb_c1203rw, tb_c1203_1, tb_c1203_1n); tb_c1203rw += tb_c1203_1n; memcpy(tb_c1203rw, tb_c1203_2, tb_c1203_2n); tb_c1203rw += tb_c1203_2n; memcpy(tb_c1203rw, tb_c1203_3, tb_c1203_3n); tb_c1203rw += tb_c1203_3n; memcpy(tb_c1203rw, tb_c1203_4, tb_c1203_4n); tb_c1203rw += tb_c1203_4n; *tb_c1203rw = 0; (const char *)tb_c1203r; }));
    {
        tb_list * tb_l1204 = ((tb_o_Node *)tb_nn(v_o, 1688))->q_kids;
        for (long long tb_i1204 = 0; tb_i1204 < tb_l1204->len; tb_i1204++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1204->data)[tb_i1204];
            f_emit__put(v_c, ({ const char *tb_c1205_0 = "    l->q_"; long long tb_c1205_0n = sizeof("    l->q_") - 1; const char *tb_c1205_1 = ((tb_o_Node *)tb_nn(v_f, 1689))->q_name; long long tb_c1205_1n = (long long)strlen(tb_c1205_1); const char *tb_c1205_2 = " = tb_mem(sizeof("; long long tb_c1205_2n = sizeof(" = tb_mem(sizeof(") - 1; const char *tb_c1205_3 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1689))->q_ty); long long tb_c1205_3n = (long long)strlen(tb_c1205_3); const char *tb_c1205_4 = ") * 4);\n"; long long tb_c1205_4n = sizeof(") * 4);\n") - 1; char *tb_c1205r = tb_mem(tb_c1205_0n + tb_c1205_1n + tb_c1205_2n + tb_c1205_3n + tb_c1205_4n + 1), *tb_c1205rw = tb_c1205r; memcpy(tb_c1205rw, tb_c1205_0, tb_c1205_0n); tb_c1205rw += tb_c1205_0n; memcpy(tb_c1205rw, tb_c1205_1, tb_c1205_1n); tb_c1205rw += tb_c1205_1n; memcpy(tb_c1205rw, tb_c1205_2, tb_c1205_2n); tb_c1205rw += tb_c1205_2n; memcpy(tb_c1205rw, tb_c1205_3, tb_c1205_3n); tb_c1205rw += tb_c1205_3n; memcpy(tb_c1205rw, tb_c1205_4, tb_c1205_4n); tb_c1205rw += tb_c1205_4n; *tb_c1205rw = 0; (const char *)tb_c1205r; }));
        }
    }
    f_emit__put(v_c, "    return l;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1206_0 = "static void tb_colgrow_"; long long tb_c1206_0n = sizeof("static void tb_colgrow_") - 1; const char *tb_c1206_1 = v_nm; long long tb_c1206_1n = (long long)strlen(tb_c1206_1); const char *tb_c1206_2 = "("; long long tb_c1206_2n = sizeof("(") - 1; const char *tb_c1206_3 = v_cols; long long tb_c1206_3n = (long long)strlen(tb_c1206_3); const char *tb_c1206_4 = " *l) {\n"; long long tb_c1206_4n = sizeof(" *l) {\n") - 1; char *tb_c1206r = tb_mem(tb_c1206_0n + tb_c1206_1n + tb_c1206_2n + tb_c1206_3n + tb_c1206_4n + 1), *tb_c1206rw = tb_c1206r; memcpy(tb_c1206rw, tb_c1206_0, tb_c1206_0n); tb_c1206rw += tb_c1206_0n; memcpy(tb_c1206rw, tb_c1206_1, tb_c1206_1n); tb_c1206rw += tb_c1206_1n; memcpy(tb_c1206rw, tb_c1206_2, tb_c1206_2n); tb_c1206rw += tb_c1206_2n; memcpy(tb_c1206rw, tb_c1206_3, tb_c1206_3n); tb_c1206rw += tb_c1206_3n; memcpy(tb_c1206rw, tb_c1206_4, tb_c1206_4n); tb_c1206rw += tb_c1206_4n; *tb_c1206rw = 0; (const char *)tb_c1206r; }));
    f_emit__put(v_c, "    if (l->len < l->cap) return;\n    l->cap *= 2;\n");
    {
        tb_list * tb_l1207 = ((tb_o_Node *)tb_nn(v_o, 1693))->q_kids;
        for (long long tb_i1207 = 0; tb_i1207 < tb_l1207->len; tb_i1207++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1207->data)[tb_i1207];
            const char * v_ft = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1694))->q_ty);
            f_emit__put(v_c, ({ const char *tb_c1208_0 = "    l->q_"; long long tb_c1208_0n = sizeof("    l->q_") - 1; const char *tb_c1208_1 = ((tb_o_Node *)tb_nn(v_f, 1695))->q_name; long long tb_c1208_1n = (long long)strlen(tb_c1208_1); const char *tb_c1208_2 = " = tb_resize(l->q_"; long long tb_c1208_2n = sizeof(" = tb_resize(l->q_") - 1; const char *tb_c1208_3 = ((tb_o_Node *)tb_nn(v_f, 1695))->q_name; long long tb_c1208_3n = (long long)strlen(tb_c1208_3); const char *tb_c1208_4 = ", sizeof("; long long tb_c1208_4n = sizeof(", sizeof(") - 1; const char *tb_c1208_5 = v_ft; long long tb_c1208_5n = (long long)strlen(tb_c1208_5); const char *tb_c1208_6 = ") * (l->cap / 2), sizeof("; long long tb_c1208_6n = sizeof(") * (l->cap / 2), sizeof(") - 1; const char *tb_c1208_7 = v_ft; long long tb_c1208_7n = (long long)strlen(tb_c1208_7); const char *tb_c1208_8 = ") * l->cap);\n"; long long tb_c1208_8n = sizeof(") * l->cap);\n") - 1; char *tb_c1208r = tb_mem(tb_c1208_0n + tb_c1208_1n + tb_c1208_2n + tb_c1208_3n + tb_c1208_4n + tb_c1208_5n + tb_c1208_6n + tb_c1208_7n + tb_c1208_8n + 1), *tb_c1208rw = tb_c1208r; memcpy(tb_c1208rw, tb_c1208_0, tb_c1208_0n); tb_c1208rw += tb_c1208_0n; memcpy(tb_c1208rw, tb_c1208_1, tb_c1208_1n); tb_c1208rw += tb_c1208_1n; memcpy(tb_c1208rw, tb_c1208_2, tb_c1208_2n); tb_c1208rw += tb_c1208_2n; memcpy(tb_c1208rw, tb_c1208_3, tb_c1208_3n); tb_c1208rw += tb_c1208_3n; memcpy(tb_c1208rw, tb_c1208_4, tb_c1208_4n); tb_c1208rw += tb_c1208_4n; memcpy(tb_c1208rw, tb_c1208_5, tb_c1208_5n); tb_c1208rw += tb_c1208_5n; memcpy(tb_c1208rw, tb_c1208_6, tb_c1208_6n); tb_c1208rw += tb_c1208_6n; memcpy(tb_c1208rw, tb_c1208_7, tb_c1208_7n); tb_c1208rw += tb_c1208_7n; memcpy(tb_c1208rw, tb_c1208_8, tb_c1208_8n); tb_c1208rw += tb_c1208_8n; *tb_c1208rw = 0; (const char *)tb_c1208r; }));
        }
    }
    f_emit__put(v_c, "}\n");
    f_emit__put(v_c, ({ const char *tb_c1209_0 = "static "; long long tb_c1209_0n = sizeof("static ") - 1; const char *tb_c1209_1 = v_row; long long tb_c1209_1n = (long long)strlen(tb_c1209_1); const char *tb_c1209_2 = " tb_colat_"; long long tb_c1209_2n = sizeof(" tb_colat_") - 1; const char *tb_c1209_3 = v_nm; long long tb_c1209_3n = (long long)strlen(tb_c1209_3); const char *tb_c1209_4 = "("; long long tb_c1209_4n = sizeof("(") - 1; const char *tb_c1209_5 = v_cols; long long tb_c1209_5n = (long long)strlen(tb_c1209_5); const char *tb_c1209_6 = " *l, long long i, int line) {\n"; long long tb_c1209_6n = sizeof(" *l, long long i, int line) {\n") - 1; char *tb_c1209r = tb_mem(tb_c1209_0n + tb_c1209_1n + tb_c1209_2n + tb_c1209_3n + tb_c1209_4n + tb_c1209_5n + tb_c1209_6n + 1), *tb_c1209rw = tb_c1209r; memcpy(tb_c1209rw, tb_c1209_0, tb_c1209_0n); tb_c1209rw += tb_c1209_0n; memcpy(tb_c1209rw, tb_c1209_1, tb_c1209_1n); tb_c1209rw += tb_c1209_1n; memcpy(tb_c1209rw, tb_c1209_2, tb_c1209_2n); tb_c1209rw += tb_c1209_2n; memcpy(tb_c1209rw, tb_c1209_3, tb_c1209_3n); tb_c1209rw += tb_c1209_3n; memcpy(tb_c1209rw, tb_c1209_4, tb_c1209_4n); tb_c1209rw += tb_c1209_4n; memcpy(tb_c1209rw, tb_c1209_5, tb_c1209_5n); tb_c1209rw += tb_c1209_5n; memcpy(tb_c1209rw, tb_c1209_6, tb_c1209_6n); tb_c1209rw += tb_c1209_6n; *tb_c1209rw = 0; (const char *)tb_c1209r; }));
    f_emit__put(v_c, ({ const char *tb_c1210_0 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)(("; long long tb_c1210_0n = sizeof("    if (TB_RARE((unsigned long long)i >= (unsigned long long)((") - 1; const char *tb_c1210_1 = v_cols; long long tb_c1210_1n = (long long)strlen(tb_c1210_1); const char *tb_c1210_2 = " *)tb_nn(l, line))->len)) tb_crash(line, \"номер за пределами списка\");\n"; long long tb_c1210_2n = sizeof(" *)tb_nn(l, line))->len)) tb_crash(line, \"номер за пределами списка\");\n") - 1; char *tb_c1210r = tb_mem(tb_c1210_0n + tb_c1210_1n + tb_c1210_2n + 1), *tb_c1210rw = tb_c1210r; memcpy(tb_c1210rw, tb_c1210_0, tb_c1210_0n); tb_c1210rw += tb_c1210_0n; memcpy(tb_c1210rw, tb_c1210_1, tb_c1210_1n); tb_c1210rw += tb_c1210_1n; memcpy(tb_c1210rw, tb_c1210_2, tb_c1210_2n); tb_c1210rw += tb_c1210_2n; *tb_c1210rw = 0; (const char *)tb_c1210r; }));
    f_emit__put(v_c, ({ const char *tb_c1211_0 = "    return ("; long long tb_c1211_0n = sizeof("    return (") - 1; const char *tb_c1211_1 = v_row; long long tb_c1211_1n = (long long)strlen(tb_c1211_1); const char *tb_c1211_2 = "){l, i};\n}\n"; long long tb_c1211_2n = sizeof("){l, i};\n}\n") - 1; char *tb_c1211r = tb_mem(tb_c1211_0n + tb_c1211_1n + tb_c1211_2n + 1), *tb_c1211rw = tb_c1211r; memcpy(tb_c1211rw, tb_c1211_0, tb_c1211_0n); tb_c1211rw += tb_c1211_0n; memcpy(tb_c1211rw, tb_c1211_1, tb_c1211_1n); tb_c1211rw += tb_c1211_1n; memcpy(tb_c1211rw, tb_c1211_2, tb_c1211_2n); tb_c1211rw += tb_c1211_2n; *tb_c1211rw = 0; (const char *)tb_c1211r; }));
    {
        tb_list * tb_l1212 = ((tb_o_Node *)tb_nn(v_o, 1700))->q_kids;
        for (long long tb_i1212 = 0; tb_i1212 < tb_l1212->len; tb_i1212++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1212->data)[tb_i1212];
            const char * v_ft = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1701))->q_ty);
            f_emit__put(v_c, ({ const char *tb_c1213_0 = "static inline "; long long tb_c1213_0n = sizeof("static inline ") - 1; const char *tb_c1213_1 = v_ft; long long tb_c1213_1n = (long long)strlen(tb_c1213_1); const char *tb_c1213_2 = " *tb_colf_"; long long tb_c1213_2n = sizeof(" *tb_colf_") - 1; const char *tb_c1213_3 = v_nm; long long tb_c1213_3n = (long long)strlen(tb_c1213_3); const char *tb_c1213_4 = "_"; long long tb_c1213_4n = sizeof("_") - 1; const char *tb_c1213_5 = ((tb_o_Node *)tb_nn(v_f, 1702))->q_name; long long tb_c1213_5n = (long long)strlen(tb_c1213_5); const char *tb_c1213_6 = "("; long long tb_c1213_6n = sizeof("(") - 1; const char *tb_c1213_7 = v_row; long long tb_c1213_7n = (long long)strlen(tb_c1213_7); const char *tb_c1213_8 = " r, int line) {\n"; long long tb_c1213_8n = sizeof(" r, int line) {\n") - 1; char *tb_c1213r = tb_mem(tb_c1213_0n + tb_c1213_1n + tb_c1213_2n + tb_c1213_3n + tb_c1213_4n + tb_c1213_5n + tb_c1213_6n + tb_c1213_7n + tb_c1213_8n + 1), *tb_c1213rw = tb_c1213r; memcpy(tb_c1213rw, tb_c1213_0, tb_c1213_0n); tb_c1213rw += tb_c1213_0n; memcpy(tb_c1213rw, tb_c1213_1, tb_c1213_1n); tb_c1213rw += tb_c1213_1n; memcpy(tb_c1213rw, tb_c1213_2, tb_c1213_2n); tb_c1213rw += tb_c1213_2n; memcpy(tb_c1213rw, tb_c1213_3, tb_c1213_3n); tb_c1213rw += tb_c1213_3n; memcpy(tb_c1213rw, tb_c1213_4, tb_c1213_4n); tb_c1213rw += tb_c1213_4n; memcpy(tb_c1213rw, tb_c1213_5, tb_c1213_5n); tb_c1213rw += tb_c1213_5n; memcpy(tb_c1213rw, tb_c1213_6, tb_c1213_6n); tb_c1213rw += tb_c1213_6n; memcpy(tb_c1213rw, tb_c1213_7, tb_c1213_7n); tb_c1213rw += tb_c1213_7n; memcpy(tb_c1213rw, tb_c1213_8, tb_c1213_8n); tb_c1213rw += tb_c1213_8n; *tb_c1213rw = 0; (const char *)tb_c1213r; }));
            f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
            f_emit__put(v_c, ({ const char *tb_c1214_0 = "    return &r.c->q_"; long long tb_c1214_0n = sizeof("    return &r.c->q_") - 1; const char *tb_c1214_1 = ((tb_o_Node *)tb_nn(v_f, 1704))->q_name; long long tb_c1214_1n = (long long)strlen(tb_c1214_1); const char *tb_c1214_2 = "[r.i];\n}\n"; long long tb_c1214_2n = sizeof("[r.i];\n}\n") - 1; char *tb_c1214r = tb_mem(tb_c1214_0n + tb_c1214_1n + tb_c1214_2n + 1), *tb_c1214rw = tb_c1214r; memcpy(tb_c1214rw, tb_c1214_0, tb_c1214_0n); tb_c1214rw += tb_c1214_0n; memcpy(tb_c1214rw, tb_c1214_1, tb_c1214_1n); tb_c1214rw += tb_c1214_1n; memcpy(tb_c1214rw, tb_c1214_2, tb_c1214_2n); tb_c1214rw += tb_c1214_2n; *tb_c1214rw = 0; (const char *)tb_c1214r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1215_0 = "static void tb_coladd_"; long long tb_c1215_0n = sizeof("static void tb_coladd_") - 1; const char *tb_c1215_1 = v_nm; long long tb_c1215_1n = (long long)strlen(tb_c1215_1); const char *tb_c1215_2 = "("; long long tb_c1215_2n = sizeof("(") - 1; const char *tb_c1215_3 = v_cols; long long tb_c1215_3n = (long long)strlen(tb_c1215_3); const char *tb_c1215_4 = " *l, "; long long tb_c1215_4n = sizeof(" *l, ") - 1; const char *tb_c1215_5 = v_row; long long tb_c1215_5n = (long long)strlen(tb_c1215_5); const char *tb_c1215_6 = " r, int line) {\n"; long long tb_c1215_6n = sizeof(" r, int line) {\n") - 1; char *tb_c1215r = tb_mem(tb_c1215_0n + tb_c1215_1n + tb_c1215_2n + tb_c1215_3n + tb_c1215_4n + tb_c1215_5n + tb_c1215_6n + 1), *tb_c1215rw = tb_c1215r; memcpy(tb_c1215rw, tb_c1215_0, tb_c1215_0n); tb_c1215rw += tb_c1215_0n; memcpy(tb_c1215rw, tb_c1215_1, tb_c1215_1n); tb_c1215rw += tb_c1215_1n; memcpy(tb_c1215rw, tb_c1215_2, tb_c1215_2n); tb_c1215rw += tb_c1215_2n; memcpy(tb_c1215rw, tb_c1215_3, tb_c1215_3n); tb_c1215rw += tb_c1215_3n; memcpy(tb_c1215rw, tb_c1215_4, tb_c1215_4n); tb_c1215rw += tb_c1215_4n; memcpy(tb_c1215rw, tb_c1215_5, tb_c1215_5n); tb_c1215rw += tb_c1215_5n; memcpy(tb_c1215rw, tb_c1215_6, tb_c1215_6n); tb_c1215rw += tb_c1215_6n; *tb_c1215rw = 0; (const char *)tb_c1215r; }));
    f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
    {
        long long tb_end1216 = ((((tb_o_Node *)tb_nn(v_o, 1707))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1216; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1217_0 = "    "; long long tb_c1217_0n = sizeof("    ") - 1; const char *tb_c1217_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1708))->q_kids, v_i, sizeof(tb_o_Node *), 1708)), 1708))->q_ty); long long tb_c1217_1n = (long long)strlen(tb_c1217_1); const char *tb_c1217_2 = " a"; long long tb_c1217_2n = sizeof(" a") - 1; char tb_c1217_3d[24]; long long tb_c1217_3n; const char *tb_c1217_3 = tb_digits(v_i, tb_c1217_3d, &tb_c1217_3n); const char *tb_c1217_4 = " = r.c->q_"; long long tb_c1217_4n = sizeof(" = r.c->q_") - 1; const char *tb_c1217_5 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1708))->q_kids, v_i, sizeof(tb_o_Node *), 1708)), 1708))->q_name; long long tb_c1217_5n = (long long)strlen(tb_c1217_5); const char *tb_c1217_6 = "[r.i];\n"; long long tb_c1217_6n = sizeof("[r.i];\n") - 1; char *tb_c1217r = tb_mem(tb_c1217_0n + tb_c1217_1n + tb_c1217_2n + tb_c1217_3n + tb_c1217_4n + tb_c1217_5n + tb_c1217_6n + 1), *tb_c1217rw = tb_c1217r; memcpy(tb_c1217rw, tb_c1217_0, tb_c1217_0n); tb_c1217rw += tb_c1217_0n; memcpy(tb_c1217rw, tb_c1217_1, tb_c1217_1n); tb_c1217rw += tb_c1217_1n; memcpy(tb_c1217rw, tb_c1217_2, tb_c1217_2n); tb_c1217rw += tb_c1217_2n; memcpy(tb_c1217rw, tb_c1217_3, tb_c1217_3n); tb_c1217rw += tb_c1217_3n; memcpy(tb_c1217rw, tb_c1217_4, tb_c1217_4n); tb_c1217rw += tb_c1217_4n; memcpy(tb_c1217rw, tb_c1217_5, tb_c1217_5n); tb_c1217rw += tb_c1217_5n; memcpy(tb_c1217rw, tb_c1217_6, tb_c1217_6n); tb_c1217rw += tb_c1217_6n; *tb_c1217rw = 0; (const char *)tb_c1217r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1218_0 = "    tb_colgrow_"; long long tb_c1218_0n = sizeof("    tb_colgrow_") - 1; const char *tb_c1218_1 = v_nm; long long tb_c1218_1n = (long long)strlen(tb_c1218_1); const char *tb_c1218_2 = "(tb_nn(l, line));\n"; long long tb_c1218_2n = sizeof("(tb_nn(l, line));\n") - 1; char *tb_c1218r = tb_mem(tb_c1218_0n + tb_c1218_1n + tb_c1218_2n + 1), *tb_c1218rw = tb_c1218r; memcpy(tb_c1218rw, tb_c1218_0, tb_c1218_0n); tb_c1218rw += tb_c1218_0n; memcpy(tb_c1218rw, tb_c1218_1, tb_c1218_1n); tb_c1218rw += tb_c1218_1n; memcpy(tb_c1218rw, tb_c1218_2, tb_c1218_2n); tb_c1218rw += tb_c1218_2n; *tb_c1218rw = 0; (const char *)tb_c1218r; }));
    {
        long long tb_end1219 = ((((tb_o_Node *)tb_nn(v_o, 1710))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1219; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1220_0 = "    l->q_"; long long tb_c1220_0n = sizeof("    l->q_") - 1; const char *tb_c1220_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1711))->q_kids, v_i, sizeof(tb_o_Node *), 1711)), 1711))->q_name; long long tb_c1220_1n = (long long)strlen(tb_c1220_1); const char *tb_c1220_2 = "[l->len] = a"; long long tb_c1220_2n = sizeof("[l->len] = a") - 1; char tb_c1220_3d[24]; long long tb_c1220_3n; const char *tb_c1220_3 = tb_digits(v_i, tb_c1220_3d, &tb_c1220_3n); const char *tb_c1220_4 = ";\n"; long long tb_c1220_4n = sizeof(";\n") - 1; char *tb_c1220r = tb_mem(tb_c1220_0n + tb_c1220_1n + tb_c1220_2n + tb_c1220_3n + tb_c1220_4n + 1), *tb_c1220rw = tb_c1220r; memcpy(tb_c1220rw, tb_c1220_0, tb_c1220_0n); tb_c1220rw += tb_c1220_0n; memcpy(tb_c1220rw, tb_c1220_1, tb_c1220_1n); tb_c1220rw += tb_c1220_1n; memcpy(tb_c1220rw, tb_c1220_2, tb_c1220_2n); tb_c1220rw += tb_c1220_2n; memcpy(tb_c1220rw, tb_c1220_3, tb_c1220_3n); tb_c1220rw += tb_c1220_3n; memcpy(tb_c1220rw, tb_c1220_4, tb_c1220_4n); tb_c1220rw += tb_c1220_4n; *tb_c1220rw = 0; (const char *)tb_c1220r; }));
        }
    }
    f_emit__put(v_c, "    l->len += 1;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1221_0 = "static void tb_colpush_"; long long tb_c1221_0n = sizeof("static void tb_colpush_") - 1; const char *tb_c1221_1 = v_nm; long long tb_c1221_1n = (long long)strlen(tb_c1221_1); const char *tb_c1221_2 = "("; long long tb_c1221_2n = sizeof("(") - 1; const char *tb_c1221_3 = v_cols; long long tb_c1221_3n = (long long)strlen(tb_c1221_3); const char *tb_c1221_4 = " *l"; long long tb_c1221_4n = sizeof(" *l") - 1; char *tb_c1221r = tb_mem(tb_c1221_0n + tb_c1221_1n + tb_c1221_2n + tb_c1221_3n + tb_c1221_4n + 1), *tb_c1221rw = tb_c1221r; memcpy(tb_c1221rw, tb_c1221_0, tb_c1221_0n); tb_c1221rw += tb_c1221_0n; memcpy(tb_c1221rw, tb_c1221_1, tb_c1221_1n); tb_c1221rw += tb_c1221_1n; memcpy(tb_c1221rw, tb_c1221_2, tb_c1221_2n); tb_c1221rw += tb_c1221_2n; memcpy(tb_c1221rw, tb_c1221_3, tb_c1221_3n); tb_c1221rw += tb_c1221_3n; memcpy(tb_c1221rw, tb_c1221_4, tb_c1221_4n); tb_c1221rw += tb_c1221_4n; *tb_c1221rw = 0; (const char *)tb_c1221r; }));
    {
        long long tb_end1222 = ((((tb_o_Node *)tb_nn(v_o, 1714))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1222; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1223_0 = ", "; long long tb_c1223_0n = sizeof(", ") - 1; const char *tb_c1223_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1715))->q_kids, v_i, sizeof(tb_o_Node *), 1715)), 1715))->q_ty); long long tb_c1223_1n = (long long)strlen(tb_c1223_1); const char *tb_c1223_2 = " a"; long long tb_c1223_2n = sizeof(" a") - 1; char tb_c1223_3d[24]; long long tb_c1223_3n; const char *tb_c1223_3 = tb_digits(v_i, tb_c1223_3d, &tb_c1223_3n); char *tb_c1223r = tb_mem(tb_c1223_0n + tb_c1223_1n + tb_c1223_2n + tb_c1223_3n + 1), *tb_c1223rw = tb_c1223r; memcpy(tb_c1223rw, tb_c1223_0, tb_c1223_0n); tb_c1223rw += tb_c1223_0n; memcpy(tb_c1223rw, tb_c1223_1, tb_c1223_1n); tb_c1223rw += tb_c1223_1n; memcpy(tb_c1223rw, tb_c1223_2, tb_c1223_2n); tb_c1223rw += tb_c1223_2n; memcpy(tb_c1223rw, tb_c1223_3, tb_c1223_3n); tb_c1223rw += tb_c1223_3n; *tb_c1223rw = 0; (const char *)tb_c1223r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1224_0 = ", int line) {\n    tb_colgrow_"; long long tb_c1224_0n = sizeof(", int line) {\n    tb_colgrow_") - 1; const char *tb_c1224_1 = v_nm; long long tb_c1224_1n = (long long)strlen(tb_c1224_1); const char *tb_c1224_2 = "(tb_nn(l, line));\n"; long long tb_c1224_2n = sizeof("(tb_nn(l, line));\n") - 1; char *tb_c1224r = tb_mem(tb_c1224_0n + tb_c1224_1n + tb_c1224_2n + 1), *tb_c1224rw = tb_c1224r; memcpy(tb_c1224rw, tb_c1224_0, tb_c1224_0n); tb_c1224rw += tb_c1224_0n; memcpy(tb_c1224rw, tb_c1224_1, tb_c1224_1n); tb_c1224rw += tb_c1224_1n; memcpy(tb_c1224rw, tb_c1224_2, tb_c1224_2n); tb_c1224rw += tb_c1224_2n; *tb_c1224rw = 0; (const char *)tb_c1224r; }));
    {
        long long tb_end1225 = ((((tb_o_Node *)tb_nn(v_o, 1717))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1225; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1226_0 = "    l->q_"; long long tb_c1226_0n = sizeof("    l->q_") - 1; const char *tb_c1226_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1718))->q_kids, v_i, sizeof(tb_o_Node *), 1718)), 1718))->q_name; long long tb_c1226_1n = (long long)strlen(tb_c1226_1); const char *tb_c1226_2 = "[l->len] = a"; long long tb_c1226_2n = sizeof("[l->len] = a") - 1; char tb_c1226_3d[24]; long long tb_c1226_3n; const char *tb_c1226_3 = tb_digits(v_i, tb_c1226_3d, &tb_c1226_3n); const char *tb_c1226_4 = ";\n"; long long tb_c1226_4n = sizeof(";\n") - 1; char *tb_c1226r = tb_mem(tb_c1226_0n + tb_c1226_1n + tb_c1226_2n + tb_c1226_3n + tb_c1226_4n + 1), *tb_c1226rw = tb_c1226r; memcpy(tb_c1226rw, tb_c1226_0, tb_c1226_0n); tb_c1226rw += tb_c1226_0n; memcpy(tb_c1226rw, tb_c1226_1, tb_c1226_1n); tb_c1226rw += tb_c1226_1n; memcpy(tb_c1226rw, tb_c1226_2, tb_c1226_2n); tb_c1226rw += tb_c1226_2n; memcpy(tb_c1226rw, tb_c1226_3, tb_c1226_3n); tb_c1226rw += tb_c1226_3n; memcpy(tb_c1226rw, tb_c1226_4, tb_c1226_4n); tb_c1226rw += tb_c1226_4n; *tb_c1226rw = 0; (const char *)tb_c1226r; }));
        }
    }
    f_emit__put(v_c, "    l->len += 1;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1227_0 = "static void tb_colset_"; long long tb_c1227_0n = sizeof("static void tb_colset_") - 1; const char *tb_c1227_1 = v_nm; long long tb_c1227_1n = (long long)strlen(tb_c1227_1); const char *tb_c1227_2 = "("; long long tb_c1227_2n = sizeof("(") - 1; const char *tb_c1227_3 = v_cols; long long tb_c1227_3n = (long long)strlen(tb_c1227_3); const char *tb_c1227_4 = " *l, long long i, "; long long tb_c1227_4n = sizeof(" *l, long long i, ") - 1; const char *tb_c1227_5 = v_row; long long tb_c1227_5n = (long long)strlen(tb_c1227_5); const char *tb_c1227_6 = " r, int line) {\n"; long long tb_c1227_6n = sizeof(" r, int line) {\n") - 1; char *tb_c1227r = tb_mem(tb_c1227_0n + tb_c1227_1n + tb_c1227_2n + tb_c1227_3n + tb_c1227_4n + tb_c1227_5n + tb_c1227_6n + 1), *tb_c1227rw = tb_c1227r; memcpy(tb_c1227rw, tb_c1227_0, tb_c1227_0n); tb_c1227rw += tb_c1227_0n; memcpy(tb_c1227rw, tb_c1227_1, tb_c1227_1n); tb_c1227rw += tb_c1227_1n; memcpy(tb_c1227rw, tb_c1227_2, tb_c1227_2n); tb_c1227rw += tb_c1227_2n; memcpy(tb_c1227rw, tb_c1227_3, tb_c1227_3n); tb_c1227rw += tb_c1227_3n; memcpy(tb_c1227rw, tb_c1227_4, tb_c1227_4n); tb_c1227rw += tb_c1227_4n; memcpy(tb_c1227rw, tb_c1227_5, tb_c1227_5n); tb_c1227rw += tb_c1227_5n; memcpy(tb_c1227rw, tb_c1227_6, tb_c1227_6n); tb_c1227rw += tb_c1227_6n; *tb_c1227rw = 0; (const char *)tb_c1227r; }));
    f_emit__put(v_c, ({ const char *tb_c1228_0 = "    tb_colat_"; long long tb_c1228_0n = sizeof("    tb_colat_") - 1; const char *tb_c1228_1 = v_nm; long long tb_c1228_1n = (long long)strlen(tb_c1228_1); const char *tb_c1228_2 = "(l, i, line);\n"; long long tb_c1228_2n = sizeof("(l, i, line);\n") - 1; char *tb_c1228r = tb_mem(tb_c1228_0n + tb_c1228_1n + tb_c1228_2n + 1), *tb_c1228rw = tb_c1228r; memcpy(tb_c1228rw, tb_c1228_0, tb_c1228_0n); tb_c1228rw += tb_c1228_0n; memcpy(tb_c1228rw, tb_c1228_1, tb_c1228_1n); tb_c1228rw += tb_c1228_1n; memcpy(tb_c1228rw, tb_c1228_2, tb_c1228_2n); tb_c1228rw += tb_c1228_2n; *tb_c1228rw = 0; (const char *)tb_c1228r; }));
    f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
    {
        tb_list * tb_l1229 = ((tb_o_Node *)tb_nn(v_o, 1723))->q_kids;
        for (long long tb_i1229 = 0; tb_i1229 < tb_l1229->len; tb_i1229++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1229->data)[tb_i1229];
            f_emit__put(v_c, ({ const char *tb_c1230_0 = "    l->q_"; long long tb_c1230_0n = sizeof("    l->q_") - 1; const char *tb_c1230_1 = ((tb_o_Node *)tb_nn(v_f, 1724))->q_name; long long tb_c1230_1n = (long long)strlen(tb_c1230_1); const char *tb_c1230_2 = "[i] = r.c->q_"; long long tb_c1230_2n = sizeof("[i] = r.c->q_") - 1; const char *tb_c1230_3 = ((tb_o_Node *)tb_nn(v_f, 1724))->q_name; long long tb_c1230_3n = (long long)strlen(tb_c1230_3); const char *tb_c1230_4 = "[r.i];\n"; long long tb_c1230_4n = sizeof("[r.i];\n") - 1; char *tb_c1230r = tb_mem(tb_c1230_0n + tb_c1230_1n + tb_c1230_2n + tb_c1230_3n + tb_c1230_4n + 1), *tb_c1230rw = tb_c1230r; memcpy(tb_c1230rw, tb_c1230_0, tb_c1230_0n); tb_c1230rw += tb_c1230_0n; memcpy(tb_c1230rw, tb_c1230_1, tb_c1230_1n); tb_c1230rw += tb_c1230_1n; memcpy(tb_c1230rw, tb_c1230_2, tb_c1230_2n); tb_c1230rw += tb_c1230_2n; memcpy(tb_c1230rw, tb_c1230_3, tb_c1230_3n); tb_c1230rw += tb_c1230_3n; memcpy(tb_c1230rw, tb_c1230_4, tb_c1230_4n); tb_c1230rw += tb_c1230_4n; *tb_c1230rw = 0; (const char *)tb_c1230r; }));
        }
    }
    f_emit__put(v_c, "}\n");
    f_emit__put(v_c, ({ const char *tb_c1231_0 = "static "; long long tb_c1231_0n = sizeof("static ") - 1; const char *tb_c1231_1 = v_cols; long long tb_c1231_1n = (long long)strlen(tb_c1231_1); const char *tb_c1231_2 = " *tb_colsfrom_"; long long tb_c1231_2n = sizeof(" *tb_colsfrom_") - 1; const char *tb_c1231_3 = v_nm; long long tb_c1231_3n = (long long)strlen(tb_c1231_3); const char *tb_c1231_4 = "(long long n, "; long long tb_c1231_4n = sizeof("(long long n, ") - 1; const char *tb_c1231_5 = v_row; long long tb_c1231_5n = (long long)strlen(tb_c1231_5); const char *tb_c1231_6 = " *rows) {\n"; long long tb_c1231_6n = sizeof(" *rows) {\n") - 1; char *tb_c1231r = tb_mem(tb_c1231_0n + tb_c1231_1n + tb_c1231_2n + tb_c1231_3n + tb_c1231_4n + tb_c1231_5n + tb_c1231_6n + 1), *tb_c1231rw = tb_c1231r; memcpy(tb_c1231rw, tb_c1231_0, tb_c1231_0n); tb_c1231rw += tb_c1231_0n; memcpy(tb_c1231rw, tb_c1231_1, tb_c1231_1n); tb_c1231rw += tb_c1231_1n; memcpy(tb_c1231rw, tb_c1231_2, tb_c1231_2n); tb_c1231rw += tb_c1231_2n; memcpy(tb_c1231rw, tb_c1231_3, tb_c1231_3n); tb_c1231rw += tb_c1231_3n; memcpy(tb_c1231rw, tb_c1231_4, tb_c1231_4n); tb_c1231rw += tb_c1231_4n; memcpy(tb_c1231rw, tb_c1231_5, tb_c1231_5n); tb_c1231rw += tb_c1231_5n; memcpy(tb_c1231rw, tb_c1231_6, tb_c1231_6n); tb_c1231rw += tb_c1231_6n; *tb_c1231rw = 0; (const char *)tb_c1231r; }));
    f_emit__put(v_c, ({ const char *tb_c1232_0 = "    "; long long tb_c1232_0n = sizeof("    ") - 1; const char *tb_c1232_1 = v_cols; long long tb_c1232_1n = (long long)strlen(tb_c1232_1); const char *tb_c1232_2 = " *l = tb_cols_new_"; long long tb_c1232_2n = sizeof(" *l = tb_cols_new_") - 1; const char *tb_c1232_3 = v_nm; long long tb_c1232_3n = (long long)strlen(tb_c1232_3); const char *tb_c1232_4 = "();\n"; long long tb_c1232_4n = sizeof("();\n") - 1; char *tb_c1232r = tb_mem(tb_c1232_0n + tb_c1232_1n + tb_c1232_2n + tb_c1232_3n + tb_c1232_4n + 1), *tb_c1232rw = tb_c1232r; memcpy(tb_c1232rw, tb_c1232_0, tb_c1232_0n); tb_c1232rw += tb_c1232_0n; memcpy(tb_c1232rw, tb_c1232_1, tb_c1232_1n); tb_c1232rw += tb_c1232_1n; memcpy(tb_c1232rw, tb_c1232_2, tb_c1232_2n); tb_c1232rw += tb_c1232_2n; memcpy(tb_c1232rw, tb_c1232_3, tb_c1232_3n); tb_c1232rw += tb_c1232_3n; memcpy(tb_c1232rw, tb_c1232_4, tb_c1232_4n); tb_c1232rw += tb_c1232_4n; *tb_c1232rw = 0; (const char *)tb_c1232r; }));
    f_emit__put(v_c, ({ const char *tb_c1233_0 = "    for (long long i = 0; i < n; i++) tb_coladd_"; long long tb_c1233_0n = sizeof("    for (long long i = 0; i < n; i++) tb_coladd_") - 1; const char *tb_c1233_1 = v_nm; long long tb_c1233_1n = (long long)strlen(tb_c1233_1); const char *tb_c1233_2 = "(l, rows[i], 0);\n"; long long tb_c1233_2n = sizeof("(l, rows[i], 0);\n") - 1; char *tb_c1233r = tb_mem(tb_c1233_0n + tb_c1233_1n + tb_c1233_2n + 1), *tb_c1233rw = tb_c1233r; memcpy(tb_c1233rw, tb_c1233_0, tb_c1233_0n); tb_c1233rw += tb_c1233_0n; memcpy(tb_c1233rw, tb_c1233_1, tb_c1233_1n); tb_c1233rw += tb_c1233_1n; memcpy(tb_c1233rw, tb_c1233_2, tb_c1233_2n); tb_c1233rw += tb_c1233_2n; *tb_c1233rw = 0; (const char *)tb_c1233r; }));
    f_emit__put(v_c, "    return l;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1234_0 = "static "; long long tb_c1234_0n = sizeof("static ") - 1; const char *tb_c1234_1 = v_row; long long tb_c1234_1n = (long long)strlen(tb_c1234_1); const char *tb_c1234_2 = " tb_new_"; long long tb_c1234_2n = sizeof(" tb_new_") - 1; const char *tb_c1234_3 = v_nm; long long tb_c1234_3n = (long long)strlen(tb_c1234_3); const char *tb_c1234_4 = "("; long long tb_c1234_4n = sizeof("(") - 1; char *tb_c1234r = tb_mem(tb_c1234_0n + tb_c1234_1n + tb_c1234_2n + tb_c1234_3n + tb_c1234_4n + 1), *tb_c1234rw = tb_c1234r; memcpy(tb_c1234rw, tb_c1234_0, tb_c1234_0n); tb_c1234rw += tb_c1234_0n; memcpy(tb_c1234rw, tb_c1234_1, tb_c1234_1n); tb_c1234rw += tb_c1234_1n; memcpy(tb_c1234rw, tb_c1234_2, tb_c1234_2n); tb_c1234rw += tb_c1234_2n; memcpy(tb_c1234rw, tb_c1234_3, tb_c1234_3n); tb_c1234rw += tb_c1234_3n; memcpy(tb_c1234rw, tb_c1234_4, tb_c1234_4n); tb_c1234rw += tb_c1234_4n; *tb_c1234rw = 0; (const char *)tb_c1234r; }));
    if (((((tb_o_Node *)tb_nn(v_o, 1732))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end1235 = ((((tb_o_Node *)tb_nn(v_o, 1734))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1235; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1236_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1737))->q_kids, v_i, sizeof(tb_o_Node *), 1737)), 1737))->q_ty); long long tb_c1236_0n = (long long)strlen(tb_c1236_0); const char *tb_c1236_1 = " a"; long long tb_c1236_1n = sizeof(" a") - 1; char tb_c1236_2d[24]; long long tb_c1236_2n; const char *tb_c1236_2 = tb_digits(v_i, tb_c1236_2d, &tb_c1236_2n); char *tb_c1236r = tb_mem(tb_c1236_0n + tb_c1236_1n + tb_c1236_2n + 1), *tb_c1236rw = tb_c1236r; memcpy(tb_c1236rw, tb_c1236_0, tb_c1236_0n); tb_c1236rw += tb_c1236_0n; memcpy(tb_c1236rw, tb_c1236_1, tb_c1236_1n); tb_c1236rw += tb_c1236_1n; memcpy(tb_c1236rw, tb_c1236_2, tb_c1236_2n); tb_c1236rw += tb_c1236_2n; *tb_c1236rw = 0; (const char *)tb_c1236r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1237_0 = ") {\n    "; long long tb_c1237_0n = sizeof(") {\n    ") - 1; const char *tb_c1237_1 = v_cols; long long tb_c1237_1n = (long long)strlen(tb_c1237_1); const char *tb_c1237_2 = " *l = tb_cols_new_"; long long tb_c1237_2n = sizeof(" *l = tb_cols_new_") - 1; const char *tb_c1237_3 = v_nm; long long tb_c1237_3n = (long long)strlen(tb_c1237_3); const char *tb_c1237_4 = "();\n"; long long tb_c1237_4n = sizeof("();\n") - 1; char *tb_c1237r = tb_mem(tb_c1237_0n + tb_c1237_1n + tb_c1237_2n + tb_c1237_3n + tb_c1237_4n + 1), *tb_c1237rw = tb_c1237r; memcpy(tb_c1237rw, tb_c1237_0, tb_c1237_0n); tb_c1237rw += tb_c1237_0n; memcpy(tb_c1237rw, tb_c1237_1, tb_c1237_1n); tb_c1237rw += tb_c1237_1n; memcpy(tb_c1237rw, tb_c1237_2, tb_c1237_2n); tb_c1237rw += tb_c1237_2n; memcpy(tb_c1237rw, tb_c1237_3, tb_c1237_3n); tb_c1237rw += tb_c1237_3n; memcpy(tb_c1237rw, tb_c1237_4, tb_c1237_4n); tb_c1237rw += tb_c1237_4n; *tb_c1237rw = 0; (const char *)tb_c1237r; }));
    {
        long long tb_end1238 = ((((tb_o_Node *)tb_nn(v_o, 1739))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1238; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1239_0 = "    l->q_"; long long tb_c1239_0n = sizeof("    l->q_") - 1; const char *tb_c1239_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1740))->q_kids, v_i, sizeof(tb_o_Node *), 1740)), 1740))->q_name; long long tb_c1239_1n = (long long)strlen(tb_c1239_1); const char *tb_c1239_2 = "[0] = a"; long long tb_c1239_2n = sizeof("[0] = a") - 1; char tb_c1239_3d[24]; long long tb_c1239_3n; const char *tb_c1239_3 = tb_digits(v_i, tb_c1239_3d, &tb_c1239_3n); const char *tb_c1239_4 = ";\n"; long long tb_c1239_4n = sizeof(";\n") - 1; char *tb_c1239r = tb_mem(tb_c1239_0n + tb_c1239_1n + tb_c1239_2n + tb_c1239_3n + tb_c1239_4n + 1), *tb_c1239rw = tb_c1239r; memcpy(tb_c1239rw, tb_c1239_0, tb_c1239_0n); tb_c1239rw += tb_c1239_0n; memcpy(tb_c1239rw, tb_c1239_1, tb_c1239_1n); tb_c1239rw += tb_c1239_1n; memcpy(tb_c1239rw, tb_c1239_2, tb_c1239_2n); tb_c1239rw += tb_c1239_2n; memcpy(tb_c1239rw, tb_c1239_3, tb_c1239_3n); tb_c1239rw += tb_c1239_3n; memcpy(tb_c1239rw, tb_c1239_4, tb_c1239_4n); tb_c1239rw += tb_c1239_4n; *tb_c1239rw = 0; (const char *)tb_c1239r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1240_0 = "    l->len = 1;\n    return ("; long long tb_c1240_0n = sizeof("    l->len = 1;\n    return (") - 1; const char *tb_c1240_1 = v_row; long long tb_c1240_1n = (long long)strlen(tb_c1240_1); const char *tb_c1240_2 = "){l, 0};\n}\n"; long long tb_c1240_2n = sizeof("){l, 0};\n}\n") - 1; char *tb_c1240r = tb_mem(tb_c1240_0n + tb_c1240_1n + tb_c1240_2n + 1), *tb_c1240rw = tb_c1240r; memcpy(tb_c1240rw, tb_c1240_0, tb_c1240_0n); tb_c1240rw += tb_c1240_0n; memcpy(tb_c1240rw, tb_c1240_1, tb_c1240_1n); tb_c1240rw += tb_c1240_1n; memcpy(tb_c1240rw, tb_c1240_2, tb_c1240_2n); tb_c1240rw += tb_c1240_2n; *tb_c1240rw = 0; (const char *)tb_c1240r; }));
}

static void f_emit__emit_constructor(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((tb_o_Node *)tb_nn(v_o, 1744))->q_by_cols) {
        f_emit__emit_cols_helpers(v_c, v_o);
        return;
    }
    f_emit__put(v_c, ({ const char *tb_c1241_0 = "static tb_o_"; long long tb_c1241_0n = sizeof("static tb_o_") - 1; const char *tb_c1241_1 = ((tb_o_Node *)tb_nn(v_o, 1747))->q_name; long long tb_c1241_1n = (long long)strlen(tb_c1241_1); const char *tb_c1241_2 = " *tb_new_"; long long tb_c1241_2n = sizeof(" *tb_new_") - 1; const char *tb_c1241_3 = ((tb_o_Node *)tb_nn(v_o, 1747))->q_name; long long tb_c1241_3n = (long long)strlen(tb_c1241_3); const char *tb_c1241_4 = "("; long long tb_c1241_4n = sizeof("(") - 1; char *tb_c1241r = tb_mem(tb_c1241_0n + tb_c1241_1n + tb_c1241_2n + tb_c1241_3n + tb_c1241_4n + 1), *tb_c1241rw = tb_c1241r; memcpy(tb_c1241rw, tb_c1241_0, tb_c1241_0n); tb_c1241rw += tb_c1241_0n; memcpy(tb_c1241rw, tb_c1241_1, tb_c1241_1n); tb_c1241rw += tb_c1241_1n; memcpy(tb_c1241rw, tb_c1241_2, tb_c1241_2n); tb_c1241rw += tb_c1241_2n; memcpy(tb_c1241rw, tb_c1241_3, tb_c1241_3n); tb_c1241rw += tb_c1241_3n; memcpy(tb_c1241rw, tb_c1241_4, tb_c1241_4n); tb_c1241rw += tb_c1241_4n; *tb_c1241rw = 0; (const char *)tb_c1241r; }));
    if (((((tb_o_Node *)tb_nn(v_o, 1748))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end1242 = ((((tb_o_Node *)tb_nn(v_o, 1750))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1242; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1243_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1753))->q_kids, v_i, sizeof(tb_o_Node *), 1753)), 1753))->q_ty); long long tb_c1243_0n = (long long)strlen(tb_c1243_0); const char *tb_c1243_1 = " a"; long long tb_c1243_1n = sizeof(" a") - 1; char tb_c1243_2d[24]; long long tb_c1243_2n; const char *tb_c1243_2 = tb_digits(v_i, tb_c1243_2d, &tb_c1243_2n); char *tb_c1243r = tb_mem(tb_c1243_0n + tb_c1243_1n + tb_c1243_2n + 1), *tb_c1243rw = tb_c1243r; memcpy(tb_c1243rw, tb_c1243_0, tb_c1243_0n); tb_c1243rw += tb_c1243_0n; memcpy(tb_c1243rw, tb_c1243_1, tb_c1243_1n); tb_c1243rw += tb_c1243_1n; memcpy(tb_c1243rw, tb_c1243_2, tb_c1243_2n); tb_c1243rw += tb_c1243_2n; *tb_c1243rw = 0; (const char *)tb_c1243r; }));
        }
    }
    f_emit__put(v_c, ") {\n");
    f_emit__put(v_c, ({ const char *tb_c1244_0 = "    tb_o_"; long long tb_c1244_0n = sizeof("    tb_o_") - 1; const char *tb_c1244_1 = ((tb_o_Node *)tb_nn(v_o, 1755))->q_name; long long tb_c1244_1n = (long long)strlen(tb_c1244_1); const char *tb_c1244_2 = " *o = tb_mem(sizeof(tb_o_"; long long tb_c1244_2n = sizeof(" *o = tb_mem(sizeof(tb_o_") - 1; const char *tb_c1244_3 = ((tb_o_Node *)tb_nn(v_o, 1755))->q_name; long long tb_c1244_3n = (long long)strlen(tb_c1244_3); const char *tb_c1244_4 = "));\n"; long long tb_c1244_4n = sizeof("));\n") - 1; char *tb_c1244r = tb_mem(tb_c1244_0n + tb_c1244_1n + tb_c1244_2n + tb_c1244_3n + tb_c1244_4n + 1), *tb_c1244rw = tb_c1244r; memcpy(tb_c1244rw, tb_c1244_0, tb_c1244_0n); tb_c1244rw += tb_c1244_0n; memcpy(tb_c1244rw, tb_c1244_1, tb_c1244_1n); tb_c1244rw += tb_c1244_1n; memcpy(tb_c1244rw, tb_c1244_2, tb_c1244_2n); tb_c1244rw += tb_c1244_2n; memcpy(tb_c1244rw, tb_c1244_3, tb_c1244_3n); tb_c1244rw += tb_c1244_3n; memcpy(tb_c1244rw, tb_c1244_4, tb_c1244_4n); tb_c1244rw += tb_c1244_4n; *tb_c1244rw = 0; (const char *)tb_c1244r; }));
    if (f_emit__has_cold(v_o)) {
        f_emit__put(v_c, ({ const char *tb_c1245_0 = "    o->tb_cold = tb_cold_mem(sizeof(struct tb_c_"; long long tb_c1245_0n = sizeof("    o->tb_cold = tb_cold_mem(sizeof(struct tb_c_") - 1; const char *tb_c1245_1 = ((tb_o_Node *)tb_nn(v_o, 1757))->q_name; long long tb_c1245_1n = (long long)strlen(tb_c1245_1); const char *tb_c1245_2 = "));\n"; long long tb_c1245_2n = sizeof("));\n") - 1; char *tb_c1245r = tb_mem(tb_c1245_0n + tb_c1245_1n + tb_c1245_2n + 1), *tb_c1245rw = tb_c1245r; memcpy(tb_c1245rw, tb_c1245_0, tb_c1245_0n); tb_c1245rw += tb_c1245_0n; memcpy(tb_c1245rw, tb_c1245_1, tb_c1245_1n); tb_c1245rw += tb_c1245_1n; memcpy(tb_c1245rw, tb_c1245_2, tb_c1245_2n); tb_c1245rw += tb_c1245_2n; *tb_c1245rw = 0; (const char *)tb_c1245r; }));
    }
    {
        long long tb_end1246 = ((((tb_o_Node *)tb_nn(v_o, 1758))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1246; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1247_0 = "    o"; long long tb_c1247_0n = sizeof("    o") - 1; const char *tb_c1247_1 = f_emit__field_path(v_o, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1759))->q_kids, v_i, sizeof(tb_o_Node *), 1759)), 1759))->q_name); long long tb_c1247_1n = (long long)strlen(tb_c1247_1); const char *tb_c1247_2 = " = a"; long long tb_c1247_2n = sizeof(" = a") - 1; char tb_c1247_3d[24]; long long tb_c1247_3n; const char *tb_c1247_3 = tb_digits(v_i, tb_c1247_3d, &tb_c1247_3n); const char *tb_c1247_4 = ";\n"; long long tb_c1247_4n = sizeof(";\n") - 1; char *tb_c1247r = tb_mem(tb_c1247_0n + tb_c1247_1n + tb_c1247_2n + tb_c1247_3n + tb_c1247_4n + 1), *tb_c1247rw = tb_c1247r; memcpy(tb_c1247rw, tb_c1247_0, tb_c1247_0n); tb_c1247rw += tb_c1247_0n; memcpy(tb_c1247rw, tb_c1247_1, tb_c1247_1n); tb_c1247rw += tb_c1247_1n; memcpy(tb_c1247rw, tb_c1247_2, tb_c1247_2n); tb_c1247rw += tb_c1247_2n; memcpy(tb_c1247rw, tb_c1247_3, tb_c1247_3n); tb_c1247rw += tb_c1247_3n; memcpy(tb_c1247rw, tb_c1247_4, tb_c1247_4n); tb_c1247rw += tb_c1247_4n; *tb_c1247rw = 0; (const char *)tb_c1247r; }));
        }
    }
    f_emit__put(v_c, "    return o;\n}\n");
}

static void f_emit__emit_c(tb_o_Comp * v_c, tb_o_Node * v_program) {
    tb_list * v_fresh = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1248 = v_fresh; tb_list * *tb_p1248 = &(((tb_o_Comp *)tb_nn(v_c, 1764))->q_out); *tb_p1248 = tb_v1248; }
    { long long tb_v1249 = 0LL; long long *tb_p1249 = &(((tb_o_Comp *)tb_nn(v_c, 1765))->q_label); *tb_p1249 = tb_v1249; }
    f_emit__put(v_c, "// Сделано компилятором Tachbit 0.7. Не правь руками: правь файл .tb\n");
    f_emit__put(v_c, f_runtime_text__runtime_text());
    {
        tb_list * tb_l1250 = ((tb_o_Node *)tb_nn(v_program, 1770))->q_kids;
        for (long long tb_i1250 = 0; tb_i1250 < tb_l1250->len; tb_i1250++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1250->data)[tb_i1250];
            if ((({ const char *tb_s1251 = ((tb_o_Node *)tb_nn(v_top, 1771))->q_kind; ((unsigned char)tb_s1251[0] == 111 && (unsigned char)tb_s1251[1] == 98 && (unsigned char)tb_s1251[2] == 106 && tb_s1251[3] == 0); }) && ((tb_o_Node *)tb_nn(v_top, 1771))->q_by_cols)) {
                f_emit__put(v_c, ({ const char *tb_c1252_0 = "typedef struct tb_cols_"; long long tb_c1252_0n = sizeof("typedef struct tb_cols_") - 1; const char *tb_c1252_1 = ((tb_o_Node *)tb_nn(v_top, 1772))->q_name; long long tb_c1252_1n = (long long)strlen(tb_c1252_1); const char *tb_c1252_2 = " tb_cols_"; long long tb_c1252_2n = sizeof(" tb_cols_") - 1; const char *tb_c1252_3 = ((tb_o_Node *)tb_nn(v_top, 1772))->q_name; long long tb_c1252_3n = (long long)strlen(tb_c1252_3); const char *tb_c1252_4 = ";\n"; long long tb_c1252_4n = sizeof(";\n") - 1; char *tb_c1252r = tb_mem(tb_c1252_0n + tb_c1252_1n + tb_c1252_2n + tb_c1252_3n + tb_c1252_4n + 1), *tb_c1252rw = tb_c1252r; memcpy(tb_c1252rw, tb_c1252_0, tb_c1252_0n); tb_c1252rw += tb_c1252_0n; memcpy(tb_c1252rw, tb_c1252_1, tb_c1252_1n); tb_c1252rw += tb_c1252_1n; memcpy(tb_c1252rw, tb_c1252_2, tb_c1252_2n); tb_c1252rw += tb_c1252_2n; memcpy(tb_c1252rw, tb_c1252_3, tb_c1252_3n); tb_c1252rw += tb_c1252_3n; memcpy(tb_c1252rw, tb_c1252_4, tb_c1252_4n); tb_c1252rw += tb_c1252_4n; *tb_c1252rw = 0; (const char *)tb_c1252r; }));
                f_emit__put(v_c, ({ const char *tb_c1253_0 = "typedef struct { tb_cols_"; long long tb_c1253_0n = sizeof("typedef struct { tb_cols_") - 1; const char *tb_c1253_1 = ((tb_o_Node *)tb_nn(v_top, 1773))->q_name; long long tb_c1253_1n = (long long)strlen(tb_c1253_1); const char *tb_c1253_2 = " *c; long long i; } tb_row_"; long long tb_c1253_2n = sizeof(" *c; long long i; } tb_row_") - 1; const char *tb_c1253_3 = ((tb_o_Node *)tb_nn(v_top, 1773))->q_name; long long tb_c1253_3n = (long long)strlen(tb_c1253_3); const char *tb_c1253_4 = ";\n"; long long tb_c1253_4n = sizeof(";\n") - 1; char *tb_c1253r = tb_mem(tb_c1253_0n + tb_c1253_1n + tb_c1253_2n + tb_c1253_3n + tb_c1253_4n + 1), *tb_c1253rw = tb_c1253r; memcpy(tb_c1253rw, tb_c1253_0, tb_c1253_0n); tb_c1253rw += tb_c1253_0n; memcpy(tb_c1253rw, tb_c1253_1, tb_c1253_1n); tb_c1253rw += tb_c1253_1n; memcpy(tb_c1253rw, tb_c1253_2, tb_c1253_2n); tb_c1253rw += tb_c1253_2n; memcpy(tb_c1253rw, tb_c1253_3, tb_c1253_3n); tb_c1253rw += tb_c1253_3n; memcpy(tb_c1253rw, tb_c1253_4, tb_c1253_4n); tb_c1253rw += tb_c1253_4n; *tb_c1253rw = 0; (const char *)tb_c1253r; }));
            } else if (({ const char *tb_s1254 = ((tb_o_Node *)tb_nn(v_top, 1774))->q_kind; ((unsigned char)tb_s1254[0] == 111 && (unsigned char)tb_s1254[1] == 98 && (unsigned char)tb_s1254[2] == 106 && tb_s1254[3] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c1255_0 = "typedef struct tb_o_"; long long tb_c1255_0n = sizeof("typedef struct tb_o_") - 1; const char *tb_c1255_1 = ((tb_o_Node *)tb_nn(v_top, 1775))->q_name; long long tb_c1255_1n = (long long)strlen(tb_c1255_1); const char *tb_c1255_2 = " tb_o_"; long long tb_c1255_2n = sizeof(" tb_o_") - 1; const char *tb_c1255_3 = ((tb_o_Node *)tb_nn(v_top, 1775))->q_name; long long tb_c1255_3n = (long long)strlen(tb_c1255_3); const char *tb_c1255_4 = ";\n"; long long tb_c1255_4n = sizeof(";\n") - 1; char *tb_c1255r = tb_mem(tb_c1255_0n + tb_c1255_1n + tb_c1255_2n + tb_c1255_3n + tb_c1255_4n + 1), *tb_c1255rw = tb_c1255r; memcpy(tb_c1255rw, tb_c1255_0, tb_c1255_0n); tb_c1255rw += tb_c1255_0n; memcpy(tb_c1255rw, tb_c1255_1, tb_c1255_1n); tb_c1255rw += tb_c1255_1n; memcpy(tb_c1255rw, tb_c1255_2, tb_c1255_2n); tb_c1255rw += tb_c1255_2n; memcpy(tb_c1255rw, tb_c1255_3, tb_c1255_3n); tb_c1255rw += tb_c1255_3n; memcpy(tb_c1255rw, tb_c1255_4, tb_c1255_4n); tb_c1255rw += tb_c1255_4n; *tb_c1255rw = 0; (const char *)tb_c1255r; }));
            }
        }
    }
    {
        tb_list * tb_l1256 = ((tb_o_Node *)tb_nn(v_program, 1776))->q_kids;
        for (long long tb_i1256 = 0; tb_i1256 < tb_l1256->len; tb_i1256++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1256->data)[tb_i1256];
            if (({ const char *tb_s1257 = ((tb_o_Node *)tb_nn(v_top, 1777))->q_kind; ((unsigned char)tb_s1257[0] == 111 && (unsigned char)tb_s1257[1] == 98 && (unsigned char)tb_s1257[2] == 106 && tb_s1257[3] == 0); })) {
                f_emit__emit_obj(v_c, v_top);
            }
        }
    }
    {
        tb_list * tb_l1258 = ((tb_o_Node *)tb_nn(v_program, 1780))->q_kids;
        for (long long tb_i1258 = 0; tb_i1258 < tb_l1258->len; tb_i1258++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1258->data)[tb_i1258];
            if ((({ const char *tb_s1259 = ((tb_o_Node *)tb_nn(v_top, 1781))->q_kind; ((unsigned char)tb_s1259[0] == 102 && (unsigned char)tb_s1259[1] == 117 && (unsigned char)tb_s1259[2] == 110 && (unsigned char)tb_s1259[3] == 99 && tb_s1259[4] == 0); }) && ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1781))->q_kids, 2LL, sizeof(tb_o_Node *), 1781)), 1781))->q_kids)->len > 1LL))) {
                f_emit__put(v_c, "typedef struct {");
                {
                    long long tb_end1260 = ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1783))->q_kids, 2LL, sizeof(tb_o_Node *), 1783)), 1783))->q_kids)->len - 1LL);
                    for (long long v_j = 0LL; v_j <= tb_end1260; v_j++) {
                        f_emit__put(v_c, ({ const char *tb_c1261_0 = " "; long long tb_c1261_0n = sizeof(" ") - 1; const char *tb_c1261_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1784))->q_kids, 2LL, sizeof(tb_o_Node *), 1784)), 1784))->q_kids, v_j, sizeof(tb_o_Node *), 1784)), 1784))->q_ty); long long tb_c1261_1n = (long long)strlen(tb_c1261_1); const char *tb_c1261_2 = " o"; long long tb_c1261_2n = sizeof(" o") - 1; char tb_c1261_3d[24]; long long tb_c1261_3n; const char *tb_c1261_3 = tb_digits(v_j, tb_c1261_3d, &tb_c1261_3n); const char *tb_c1261_4 = ";"; long long tb_c1261_4n = sizeof(";") - 1; char *tb_c1261r = tb_mem(tb_c1261_0n + tb_c1261_1n + tb_c1261_2n + tb_c1261_3n + tb_c1261_4n + 1), *tb_c1261rw = tb_c1261r; memcpy(tb_c1261rw, tb_c1261_0, tb_c1261_0n); tb_c1261rw += tb_c1261_0n; memcpy(tb_c1261rw, tb_c1261_1, tb_c1261_1n); tb_c1261rw += tb_c1261_1n; memcpy(tb_c1261rw, tb_c1261_2, tb_c1261_2n); tb_c1261rw += tb_c1261_2n; memcpy(tb_c1261rw, tb_c1261_3, tb_c1261_3n); tb_c1261rw += tb_c1261_3n; memcpy(tb_c1261rw, tb_c1261_4, tb_c1261_4n); tb_c1261rw += tb_c1261_4n; *tb_c1261rw = 0; (const char *)tb_c1261r; }));
                    }
                }
                f_emit__put(v_c, ({ const char *tb_c1262_0 = " } tb_r_"; long long tb_c1262_0n = sizeof(" } tb_r_") - 1; const char *tb_c1262_1 = f_emit__fname(v_top); long long tb_c1262_1n = (long long)strlen(tb_c1262_1); const char *tb_c1262_2 = ";\n"; long long tb_c1262_2n = sizeof(";\n") - 1; char *tb_c1262r = tb_mem(tb_c1262_0n + tb_c1262_1n + tb_c1262_2n + 1), *tb_c1262rw = tb_c1262r; memcpy(tb_c1262rw, tb_c1262_0, tb_c1262_0n); tb_c1262rw += tb_c1262_0n; memcpy(tb_c1262rw, tb_c1262_1, tb_c1262_1n); tb_c1262rw += tb_c1262_1n; memcpy(tb_c1262rw, tb_c1262_2, tb_c1262_2n); tb_c1262rw += tb_c1262_2n; *tb_c1262rw = 0; (const char *)tb_c1262r; }));
            }
        }
    }
    {
        tb_list * tb_l1263 = ((tb_o_Comp *)tb_nn(v_c, 1786))->q_types;
        for (long long tb_i1263 = 0; tb_i1263 < tb_l1263->len; tb_i1263++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l1263->data)[tb_i1263];
            const char * v_k = ((tb_o_Type *)tb_nn(v_t, 1787))->q_kind;
            if (((({ const char *tb_s1264 = v_k; !((unsigned char)tb_s1264[0] == 110 && (unsigned char)tb_s1264[1] == 111 && (unsigned char)tb_s1264[2] == 110 && (unsigned char)tb_s1264[3] == 101 && tb_s1264[4] == 0); }) && ({ const char *tb_s1265 = v_k; !((unsigned char)tb_s1265[0] == 109 && (unsigned char)tb_s1265[1] == 117 && (unsigned char)tb_s1265[2] == 108 && (unsigned char)tb_s1265[3] == 116 && (unsigned char)tb_s1265[4] == 105 && tb_s1265[5] == 0); })) && ({ const char *tb_s1266 = v_k; !((unsigned char)tb_s1266[0] == 117 && (unsigned char)tb_s1266[1] == 110 && (unsigned char)tb_s1266[2] == 107 && (unsigned char)tb_s1266[3] == 110 && (unsigned char)tb_s1266[4] == 111 && (unsigned char)tb_s1266[5] == 119 && (unsigned char)tb_s1266[6] == 110 && tb_s1266[7] == 0); }))) {
                f_emit__put(v_c, ({ const char *tb_c1267_0 = "static void tb_show_"; long long tb_c1267_0n = sizeof("static void tb_show_") - 1; char tb_c1267_1d[24]; long long tb_c1267_1n; const char *tb_c1267_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1789))->q_id, tb_c1267_1d, &tb_c1267_1n); const char *tb_c1267_2 = "("; long long tb_c1267_2n = sizeof("(") - 1; const char *tb_c1267_3 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1789))->q_id); long long tb_c1267_3n = (long long)strlen(tb_c1267_3); const char *tb_c1267_4 = " x);\n"; long long tb_c1267_4n = sizeof(" x);\n") - 1; char *tb_c1267r = tb_mem(tb_c1267_0n + tb_c1267_1n + tb_c1267_2n + tb_c1267_3n + tb_c1267_4n + 1), *tb_c1267rw = tb_c1267r; memcpy(tb_c1267rw, tb_c1267_0, tb_c1267_0n); tb_c1267rw += tb_c1267_0n; memcpy(tb_c1267rw, tb_c1267_1, tb_c1267_1n); tb_c1267rw += tb_c1267_1n; memcpy(tb_c1267rw, tb_c1267_2, tb_c1267_2n); tb_c1267rw += tb_c1267_2n; memcpy(tb_c1267rw, tb_c1267_3, tb_c1267_3n); tb_c1267rw += tb_c1267_3n; memcpy(tb_c1267rw, tb_c1267_4, tb_c1267_4n); tb_c1267rw += tb_c1267_4n; *tb_c1267rw = 0; (const char *)tb_c1267r; }));
            }
        }
    }
    {
        long long tb_end1268 = ((((tb_o_Comp *)tb_nn(v_c, 1790))->q_temp_types)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end1268; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1269_0 = "static _Thread_local "; long long tb_c1269_0n = sizeof("static _Thread_local ") - 1; const char *tb_c1269_1 = f_emit__c_type(v_c, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1791))->q_temp_types, v_i, sizeof(long long), 1791))); long long tb_c1269_1n = (long long)strlen(tb_c1269_1); const char *tb_c1269_2 = " tb_t"; long long tb_c1269_2n = sizeof(" tb_t") - 1; char tb_c1269_3d[24]; long long tb_c1269_3n; const char *tb_c1269_3 = tb_digits(v_i, tb_c1269_3d, &tb_c1269_3n); const char *tb_c1269_4 = ";   // для  f() or запасное\n"; long long tb_c1269_4n = sizeof(";   // для  f() or запасное\n") - 1; char *tb_c1269r = tb_mem(tb_c1269_0n + tb_c1269_1n + tb_c1269_2n + tb_c1269_3n + tb_c1269_4n + 1), *tb_c1269rw = tb_c1269r; memcpy(tb_c1269rw, tb_c1269_0, tb_c1269_0n); tb_c1269rw += tb_c1269_0n; memcpy(tb_c1269rw, tb_c1269_1, tb_c1269_1n); tb_c1269rw += tb_c1269_1n; memcpy(tb_c1269rw, tb_c1269_2, tb_c1269_2n); tb_c1269rw += tb_c1269_2n; memcpy(tb_c1269rw, tb_c1269_3, tb_c1269_3n); tb_c1269rw += tb_c1269_3n; memcpy(tb_c1269rw, tb_c1269_4, tb_c1269_4n); tb_c1269rw += tb_c1269_4n; *tb_c1269rw = 0; (const char *)tb_c1269r; }));
        }
    }
    {
        tb_list * tb_l1270 = ((tb_o_Node *)tb_nn(v_program, 1792))->q_kids;
        for (long long tb_i1270 = 0; tb_i1270 < tb_l1270->len; tb_i1270++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1270->data)[tb_i1270];
            if (({ const char *tb_s1271 = ((tb_o_Node *)tb_nn(v_top, 1793))->q_kind; ((unsigned char)tb_s1271[0] == 111 && (unsigned char)tb_s1271[1] == 98 && (unsigned char)tb_s1271[2] == 106 && tb_s1271[3] == 0); })) {
                f_emit__emit_constructor(v_c, v_top);
            }
        }
    }
    {
        tb_list * tb_l1272 = ((tb_o_Node *)tb_nn(v_program, 1795))->q_kids;
        for (long long tb_i1272 = 0; tb_i1272 < tb_l1272->len; tb_i1272++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1272->data)[tb_i1272];
            if (({ const char *tb_s1273 = ((tb_o_Node *)tb_nn(v_top, 1796))->q_kind; ((unsigned char)tb_s1273[0] == 102 && (unsigned char)tb_s1273[1] == 117 && (unsigned char)tb_s1273[2] == 110 && (unsigned char)tb_s1273[3] == 99 && tb_s1273[4] == 0); })) {
                f_emit__emit_signature(v_c, v_top);
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__put(v_c, "\n");
    {
        tb_list * tb_l1274 = ((tb_o_Comp *)tb_nn(v_c, 1800))->q_types;
        for (long long tb_i1274 = 0; tb_i1274 < tb_l1274->len; tb_i1274++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l1274->data)[tb_i1274];
            f_emit__emit_show(v_c, v_t);
        }
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 1803))->q_out;
    tb_list * v_funcs_out = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1275 = v_funcs_out; tb_list * *tb_p1275 = &(((tb_o_Comp *)tb_nn(v_c, 1805))->q_out); *tb_p1275 = tb_v1275; }
    {
        tb_list * tb_l1276 = ((tb_o_Node *)tb_nn(v_program, 1806))->q_kids;
        for (long long tb_i1276 = 0; tb_i1276 < tb_l1276->len; tb_i1276++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1276->data)[tb_i1276];
            if (({ const char *tb_s1277 = ((tb_o_Node *)tb_nn(v_top, 1807))->q_kind; ((unsigned char)tb_s1277[0] == 102 && (unsigned char)tb_s1277[1] == 117 && (unsigned char)tb_s1277[2] == 110 && (unsigned char)tb_s1277[3] == 99 && tb_s1277[4] == 0); })) {
                f_emit__emit_func(v_c, v_top);
            }
        }
    }
    { tb_list * tb_v1278 = v_saved; tb_list * *tb_p1278 = &(((tb_o_Comp *)tb_nn(v_c, 1809))->q_out); *tb_p1278 = tb_v1278; }
    {
        tb_list * tb_l1279 = ((tb_o_Comp *)tb_nn(v_c, 1810))->q_workers;
        for (long long tb_i1279 = 0; tb_i1279 < tb_l1279->len; tb_i1279++) {
            const char * const v_w = ((const char * *)tb_l1279->data)[tb_i1279];
            f_emit__put(v_c, v_w);
        }
    }
    {
        tb_list * tb_l1280 = v_funcs_out;
        for (long long tb_i1280 = 0; tb_i1280 < tb_l1280->len; tb_i1280++) {
            const char * const v_f = ((const char * *)tb_l1280->data)[tb_i1280];
            f_emit__put(v_c, v_f);
        }
    }
    tb_o_Node * v_main_func = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1814))->q_kids, 0LL, sizeof(tb_o_Node *), 1814));
    {
        tb_list * tb_l1281 = ((tb_o_Node *)tb_nn(v_program, 1815))->q_kids;
        for (long long tb_i1281 = 0; tb_i1281 < tb_l1281->len; tb_i1281++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1281->data)[tb_i1281];
            if (((({ const char *tb_s1282 = ((tb_o_Node *)tb_nn(v_top, 1816))->q_kind; ((unsigned char)tb_s1282[0] == 102 && (unsigned char)tb_s1282[1] == 117 && (unsigned char)tb_s1282[2] == 110 && (unsigned char)tb_s1282[3] == 99 && tb_s1282[4] == 0); }) && ({ const char *tb_s1283 = ((tb_o_Node *)tb_nn(v_top, 1816))->q_name; ((unsigned char)tb_s1283[0] == 109 && (unsigned char)tb_s1283[1] == 97 && (unsigned char)tb_s1283[2] == 105 && (unsigned char)tb_s1283[3] == 110 && tb_s1283[4] == 0); })) && (strcmp(((tb_o_Node *)tb_nn(v_top, 1816))->q_mod, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1816))->q_kids, 0LL, sizeof(tb_o_Node *), 1816)), 1816))->q_mod) == 0))) {
                v_main_func = v_top;
            }
        }
    }
    f_emit__put(v_c, "\nint main(int argc, char **argv) {\n");
    f_emit__put(v_c, "    tb_args = tb_list_new(sizeof(const char *));\n");
    f_emit__put(v_c, "    for (int i = 1; i < argc; i++) *(const char **)tb_grow(tb_args) = argv[i];\n");
    f_emit__put(v_c, ({ const char *tb_c1284_0 = "    "; long long tb_c1284_0n = sizeof("    ") - 1; const char *tb_c1284_1 = f_emit__fname(v_main_func); long long tb_c1284_1n = (long long)strlen(tb_c1284_1); const char *tb_c1284_2 = "();\n    return 0;\n}\n"; long long tb_c1284_2n = sizeof("();\n    return 0;\n}\n") - 1; char *tb_c1284r = tb_mem(tb_c1284_0n + tb_c1284_1n + tb_c1284_2n + 1), *tb_c1284rw = tb_c1284r; memcpy(tb_c1284rw, tb_c1284_0, tb_c1284_0n); tb_c1284rw += tb_c1284_0n; memcpy(tb_c1284rw, tb_c1284_1, tb_c1284_1n); tb_c1284rw += tb_c1284_1n; memcpy(tb_c1284rw, tb_c1284_2, tb_c1284_2n); tb_c1284rw += tb_c1284_2n; *tb_c1284rw = 0; (const char *)tb_c1284r; }));
}

static const char * f_runtime_text__runtime_text(void) {
    tb_list * v_lines = tb_list_new(sizeof(const char *));
    { const char * tb_v1285 = "// Библиотека, которую Tachbit кладёт в начало каждой программы на C.\n"; *(const char * *)tb_grow(v_lines) = tb_v1285; }
    { const char * tb_v1286 = "// Это обычный C. Компилятор на Tachbit встраивает этот текст через runtime_text.tb,\n"; *(const char * *)tb_grow(v_lines) = tb_v1286; }
    { const char * tb_v1287 = "// который делает tools/embed_runtime.py. Поменял этот файл — запусти:\n"; *(const char * *)tb_grow(v_lines) = tb_v1287; }
    { const char * tb_v1288 = "//     python3 tools/embed_runtime.py\n"; *(const char * *)tb_grow(v_lines) = tb_v1288; }
    { const char * tb_v1289 = "#include <stdio.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1289; }
    { const char * tb_v1290 = "#include <stdlib.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1290; }
    { const char * tb_v1291 = "#include <string.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1291; }
    { const char * tb_v1292 = "#include <math.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1292; }
    { const char * tb_v1293 = "#include <pthread.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1293; }
    { const char * tb_v1294 = "#include <unistd.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1294; }
    { const char * tb_v1295 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1295; }
    { const char * tb_v1296 = "// подсказки для clang / gcc: ошибка бывает редко, а tb_crash не возвращается\n"; *(const char * *)tb_grow(v_lines) = tb_v1296; }
    { const char * tb_v1297 = "#if defined(__GNUC__)\n"; *(const char * *)tb_grow(v_lines) = tb_v1297; }
    { const char * tb_v1298 = "#define TB_RARE(x) __builtin_expect(!!(x), 0)\n"; *(const char * *)tb_grow(v_lines) = tb_v1298; }
    { const char * tb_v1299 = "#define TB_COLD __attribute__((cold, noreturn, noinline))\n"; *(const char * *)tb_grow(v_lines) = tb_v1299; }
    { const char * tb_v1300 = "#else\n"; *(const char * *)tb_grow(v_lines) = tb_v1300; }
    { const char * tb_v1301 = "#define TB_RARE(x) (x)\n"; *(const char * *)tb_grow(v_lines) = tb_v1301; }
    { const char * tb_v1302 = "#define TB_COLD\n"; *(const char * *)tb_grow(v_lines) = tb_v1302; }
    { const char * tb_v1303 = "#endif\n"; *(const char * *)tb_grow(v_lines) = tb_v1303; }
    { const char * tb_v1304 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1304; }
    { const char * tb_v1305 = "// список: кусок памяти, len элементов по size байт, место на cap\n"; *(const char * *)tb_grow(v_lines) = tb_v1305; }
    { const char * tb_v1306 = "typedef struct { char *data; long long len; long long cap; long long size; } tb_list;\n"; *(const char * *)tb_grow(v_lines) = tb_v1306; }
    { const char * tb_v1307 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1307; }
    { const char * tb_v1308 = "TB_COLD static void tb_crash(int line, const char *msg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1308; }
    { const char * tb_v1309 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v1309; }
    { const char * tb_v1310 = "    // в терминале — красным (если не задан NO_COLOR)\n"; *(const char * *)tb_grow(v_lines) = tb_v1310; }
    { const char * tb_v1311 = "    int color = isatty(2) && getenv(\"NO_COLOR\") == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1311; }
    { const char * tb_v1312 = "    if (color) fputs(\"\\033[31m\", stderr);\n"; *(const char * *)tb_grow(v_lines) = tb_v1312; }
    { const char * tb_v1313 = "    if (line > 0) fprintf(stderr, \"строка %d: %s\", line, msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v1313; }
    { const char * tb_v1314 = "    else fprintf(stderr, \"%s\", msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v1314; }
    { const char * tb_v1315 = "    fputs(color ? \"\\033[0m\\n\" : \"\\n\", stderr);\n"; *(const char * *)tb_grow(v_lines) = tb_v1315; }
    { const char * tb_v1316 = "    exit(1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1316; }
    { const char * tb_v1317 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1317; }
    { const char * tb_v1318 = "// ---------- temp: временная память ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1318; }
    { const char * tb_v1319 = "// Внутри блока temp память берётся из больших кусков подряд (быстро, без malloc),\n"; *(const char * *)tb_grow(v_lines) = tb_v1319; }
    { const char * tb_v1320 = "// а в конце блока все куски отдаются разом. У каждого ядра свои куски.\n"; *(const char * *)tb_grow(v_lines) = tb_v1320; }
    { const char * tb_v1321 = "typedef struct { char *mem; long long size, used; } tb_chunk;\n"; *(const char * *)tb_grow(v_lines) = tb_v1321; }
    { const char * tb_v1322 = "typedef struct { int n; long long used; } tb_mark;\n"; *(const char * *)tb_grow(v_lines) = tb_v1322; }
    { const char * tb_v1323 = "static _Thread_local tb_chunk *tb_chunks = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1323; }
    { const char * tb_v1324 = "static _Thread_local int tb_nchunks = 0, tb_capchunks = 0, tb_temp_depth = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1324; }
    { const char * tb_v1325 = "static int tb_in_temp(const void *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1325; }
    { const char * tb_v1326 = "    for (int i = tb_nchunks - 1; i >= 0; i--) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1326; }
    { const char * tb_v1327 = "        if ((const char *)p >= tb_chunks[i].mem && (const char *)p < tb_chunks[i].mem + tb_chunks[i].size) return 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1327; }
    { const char * tb_v1328 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1328; }
    { const char * tb_v1329 = "    return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1329; }
    { const char * tb_v1330 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1330; }
    { const char * tb_v1331 = "static void *tb_temp_alloc(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1331; }
    { const char * tb_v1332 = "    n = n > 0 ? (n + 15) & ~15LL : 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v1332; }
    { const char * tb_v1333 = "    tb_chunk *c = tb_nchunks > 0 ? &tb_chunks[tb_nchunks - 1] : 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1333; }
    { const char * tb_v1334 = "    if (c == 0 || c->used + n > c->size) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1334; }
    { const char * tb_v1335 = "        long long size = c != 0 ? c->size * 2 : (1LL << 20);\n"; *(const char * *)tb_grow(v_lines) = tb_v1335; }
    { const char * tb_v1336 = "        if (size < n) size = n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1336; }
    { const char * tb_v1337 = "        if (tb_nchunks == tb_capchunks) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1337; }
    { const char * tb_v1338 = "            tb_capchunks = tb_capchunks * 2 + 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1338; }
    { const char * tb_v1339 = "            tb_chunks = realloc(tb_chunks, sizeof(tb_chunk) * tb_capchunks);\n"; *(const char * *)tb_grow(v_lines) = tb_v1339; }
    { const char * tb_v1340 = "            if (tb_chunks == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1340; }
    { const char * tb_v1341 = "        }\n"; *(const char * *)tb_grow(v_lines) = tb_v1341; }
    { const char * tb_v1342 = "        char *mem = malloc(size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1342; }
    { const char * tb_v1343 = "        if (mem == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1343; }
    { const char * tb_v1344 = "        tb_chunks[tb_nchunks] = (tb_chunk){mem, size, 0};\n"; *(const char * *)tb_grow(v_lines) = tb_v1344; }
    { const char * tb_v1345 = "        c = &tb_chunks[tb_nchunks];\n"; *(const char * *)tb_grow(v_lines) = tb_v1345; }
    { const char * tb_v1346 = "        tb_nchunks += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1346; }
    { const char * tb_v1347 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1347; }
    { const char * tb_v1348 = "    void *p = c->mem + c->used;\n"; *(const char * *)tb_grow(v_lines) = tb_v1348; }
    { const char * tb_v1349 = "    c->used += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1349; }
    { const char * tb_v1350 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1350; }
    { const char * tb_v1351 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1351; }
    { const char * tb_v1352 = "static tb_mark tb_temp_begin(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1352; }
    { const char * tb_v1353 = "    tb_temp_depth += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1353; }
    { const char * tb_v1354 = "    return (tb_mark){tb_nchunks, tb_nchunks > 0 ? tb_chunks[tb_nchunks - 1].used : 0};\n"; *(const char * *)tb_grow(v_lines) = tb_v1354; }
    { const char * tb_v1355 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1355; }
    { const char * tb_v1356 = "// длина последних текстов: s[i] в цикле не пересчитывает strlen каждый раз\n"; *(const char * *)tb_grow(v_lines) = tb_v1356; }
    { const char * tb_v1357 = "static _Thread_local const char *tb_lc_p[2];\n"; *(const char * *)tb_grow(v_lines) = tb_v1357; }
    { const char * tb_v1358 = "static _Thread_local long long tb_lc_n[2];\n"; *(const char * *)tb_grow(v_lines) = tb_v1358; }
    { const char * tb_v1359 = "static _Thread_local int tb_lc_next;\n"; *(const char * *)tb_grow(v_lines) = tb_v1359; }
    { const char * tb_v1360 = "static inline void tb_lc_clear(void) { tb_lc_p[0] = 0; tb_lc_p[1] = 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1360; }
    { const char * tb_v1361 = "static void tb_temp_end(tb_mark m) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1361; }
    { const char * tb_v1362 = "    // первый кусок оставляем пустым: следующий temp возьмёт его без malloc\n"; *(const char * *)tb_grow(v_lines) = tb_v1362; }
    { const char * tb_v1363 = "    while (tb_nchunks > m.n && tb_nchunks > 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1363; }
    { const char * tb_v1364 = "        tb_nchunks -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1364; }
    { const char * tb_v1365 = "        free(tb_chunks[tb_nchunks].mem);\n"; *(const char * *)tb_grow(v_lines) = tb_v1365; }
    { const char * tb_v1366 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1366; }
    { const char * tb_v1367 = "    if (tb_nchunks > m.n) tb_chunks[0].used = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1367; }
    { const char * tb_v1368 = "    else if (tb_nchunks > 0) tb_chunks[tb_nchunks - 1].used = m.used;\n"; *(const char * *)tb_grow(v_lines) = tb_v1368; }
    { const char * tb_v1369 = "    tb_temp_depth -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1369; }
    { const char * tb_v1370 = "    tb_lc_clear();\n"; *(const char * *)tb_grow(v_lines) = tb_v1370; }
    { const char * tb_v1371 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1371; }
    { const char * tb_v1372 = "static void *tb_mem(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1372; }
    { const char * tb_v1373 = "    if (tb_temp_depth > 0) return tb_temp_alloc(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1373; }
    { const char * tb_v1374 = "    void *p = malloc(n > 0 ? n : 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1374; }
    { const char * tb_v1375 = "    if (p == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1375; }
    { const char * tb_v1376 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1376; }
    { const char * tb_v1377 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1377; }
    { const char * tb_v1378 = "// вырасти кусок: временный растёт во временной памяти, обычный — через realloc\n"; *(const char * *)tb_grow(v_lines) = tb_v1378; }
    { const char * tb_v1379 = "static void *tb_resize(void *p, long long old_n, long long new_n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1379; }
    { const char * tb_v1380 = "    tb_lc_clear();\n"; *(const char * *)tb_grow(v_lines) = tb_v1380; }
    { const char * tb_v1381 = "    if (tb_nchunks > 0 && tb_in_temp(p)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1381; }
    { const char * tb_v1382 = "        void *q = tb_temp_alloc(new_n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1382; }
    { const char * tb_v1383 = "        memcpy(q, p, old_n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1383; }
    { const char * tb_v1384 = "        return q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1384; }
    { const char * tb_v1385 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1385; }
    { const char * tb_v1386 = "    p = realloc(p, new_n > 0 ? new_n : 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1386; }
    { const char * tb_v1387 = "    if (p == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1387; }
    { const char * tb_v1388 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1388; }
    { const char * tb_v1389 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1389; }
    { const char * tb_v1390 = "static tb_list *tb_list_new(long long size) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1390; }
    { const char * tb_v1391 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1391; }
    { const char * tb_v1392 = "    l->len = 0; l->cap = 4; l->size = size; l->data = tb_mem(size * 4);\n"; *(const char * *)tb_grow(v_lines) = tb_v1392; }
    { const char * tb_v1393 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1393; }
    { const char * tb_v1394 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1394; }
    { const char * tb_v1395 = "static tb_list *tb_list_from(long long n, long long size, const void *src) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1395; }
    { const char * tb_v1396 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1396; }
    { const char * tb_v1397 = "    l->len = n; l->cap = n; l->size = size; l->data = tb_mem(size * n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1397; }
    { const char * tb_v1398 = "    memcpy(l->data, src, size * n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1398; }
    { const char * tb_v1399 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1399; }
    { const char * tb_v1400 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1400; }
    { const char * tb_v1401 = "// size передаётся как sizeof(T): после встраивания C знает его заранее\n"; *(const char * *)tb_grow(v_lines) = tb_v1401; }
    { const char * tb_v1402 = "static inline void *tb_at(tb_list *l, long long i, long long size, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1402; }
    { const char * tb_v1403 = "    // одно сравнение вместо двух: отрицательный номер как unsigned становится огромным\n"; *(const char * *)tb_grow(v_lines) = tb_v1403; }
    { const char * tb_v1404 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1404; }
    { const char * tb_v1405 = "    return l->data + i * size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1405; }
    { const char * tb_v1406 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1406; }
    { const char * tb_v1407 = "static inline void *tb_grow(tb_list *l) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1407; }
    { const char * tb_v1408 = "    // cap < 0: это кусок (each piece in data by N) или список, который сейчас режут на куски\n"; *(const char * *)tb_grow(v_lines) = tb_v1408; }
    { const char * tb_v1409 = "    if (TB_RARE(l->len >= l->cap)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1409; }
    { const char * tb_v1410 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1410; }
    { const char * tb_v1411 = "        long long old = l->cap * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1411; }
    { const char * tb_v1412 = "        l->cap = l->cap * 2 + 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v1412; }
    { const char * tb_v1413 = "        // realloc часто растит большой кусок на месте, без копирования\n"; *(const char * *)tb_grow(v_lines) = tb_v1413; }
    { const char * tb_v1414 = "        l->data = tb_resize(l->data, old, l->cap * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1414; }
    { const char * tb_v1415 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1415; }
    { const char * tb_v1416 = "    l->len += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1416; }
    { const char * tb_v1417 = "    return l->data + (l->len - 1) * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1417; }
    { const char * tb_v1418 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1418; }
    { const char * tb_v1419 = "static void *tb_pop(tb_list *l, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1419; }
    { const char * tb_v1420 = "    if (l->len == 0) tb_crash(line, \".pop() у пустого списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1420; }
    { const char * tb_v1421 = "    l->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1421; }
    { const char * tb_v1422 = "    return l->data + l->len * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1422; }
    { const char * tb_v1423 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1423; }
    { const char * tb_v1424 = "// ---------- list bool: по одному биту на элемент (в 8 раз меньше памяти) ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1424; }
    { const char * tb_v1425 = "// тот же tb_list: len и cap — в битах, size = 0, data — слова по 64 бита\n"; *(const char * *)tb_grow(v_lines) = tb_v1425; }
    { const char * tb_v1426 = "static tb_list *tb_bits_new(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1426; }
    { const char * tb_v1427 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1427; }
    { const char * tb_v1428 = "    l->len = 0; l->cap = 64; l->size = 0; l->data = tb_mem(8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1428; }
    { const char * tb_v1429 = "    memset(l->data, 0, 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1429; }
    { const char * tb_v1430 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1430; }
    { const char * tb_v1431 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1431; }
    { const char * tb_v1432 = "static tb_list *tb_bits_from(long long n, const unsigned char *src) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1432; }
    { const char * tb_v1433 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1433; }
    { const char * tb_v1434 = "    long long words = (n + 63) / 64 + (n == 0);\n"; *(const char * *)tb_grow(v_lines) = tb_v1434; }
    { const char * tb_v1435 = "    l->len = n; l->cap = words * 64; l->size = 0; l->data = tb_mem(words * 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1435; }
    { const char * tb_v1436 = "    memset(l->data, 0, words * 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1436; }
    { const char * tb_v1437 = "    unsigned long long *w = (unsigned long long *)l->data;\n"; *(const char * *)tb_grow(v_lines) = tb_v1437; }
    { const char * tb_v1438 = "    for (long long i = 0; i < n; i++) if (src[i]) w[i >> 6] |= 1ULL << (i & 63);\n"; *(const char * *)tb_grow(v_lines) = tb_v1438; }
    { const char * tb_v1439 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1439; }
    { const char * tb_v1440 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1440; }
    { const char * tb_v1441 = "// бит без проверки номера (для each — номер всегда внутри)\n"; *(const char * *)tb_grow(v_lines) = tb_v1441; }
    { const char * tb_v1442 = "#define TB_BIT(l, i) ((_Bool)((((unsigned long long *)(l)->data)[(i) >> 6] >> ((i) & 63)) & 1))\n"; *(const char * *)tb_grow(v_lines) = tb_v1442; }
    { const char * tb_v1443 = "static inline _Bool tb_bget(tb_list *l, long long i, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1443; }
    { const char * tb_v1444 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1444; }
    { const char * tb_v1445 = "    return TB_BIT(l, i);\n"; *(const char * *)tb_grow(v_lines) = tb_v1445; }
    { const char * tb_v1446 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1446; }
    { const char * tb_v1447 = "static inline void tb_bset(tb_list *l, long long i, _Bool v, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1447; }
    { const char * tb_v1448 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1448; }
    { const char * tb_v1449 = "    unsigned long long *w = (unsigned long long *)l->data + (i >> 6), m = 1ULL << (i & 63);\n"; *(const char * *)tb_grow(v_lines) = tb_v1449; }
    { const char * tb_v1450 = "    *w = v ? (*w | m) : (*w & ~m);\n"; *(const char * *)tb_grow(v_lines) = tb_v1450; }
    { const char * tb_v1451 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1451; }
    { const char * tb_v1452 = "static inline void tb_badd(tb_list *l, _Bool v) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1452; }
    { const char * tb_v1453 = "    if (TB_RARE(l->len >= l->cap)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1453; }
    { const char * tb_v1454 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1454; }
    { const char * tb_v1455 = "        long long old = l->cap / 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1455; }
    { const char * tb_v1456 = "        l->cap = l->cap * 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1456; }
    { const char * tb_v1457 = "        l->data = tb_resize(l->data, old, l->cap / 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1457; }
    { const char * tb_v1458 = "        memset(l->data + old, 0, l->cap / 8 - old);\n"; *(const char * *)tb_grow(v_lines) = tb_v1458; }
    { const char * tb_v1459 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1459; }
    { const char * tb_v1460 = "    long long i = l->len++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1460; }
    { const char * tb_v1461 = "    unsigned long long *w = (unsigned long long *)l->data + (i >> 6), m = 1ULL << (i & 63);\n"; *(const char * *)tb_grow(v_lines) = tb_v1461; }
    { const char * tb_v1462 = "    *w = v ? (*w | m) : (*w & ~m);\n"; *(const char * *)tb_grow(v_lines) = tb_v1462; }
    { const char * tb_v1463 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1463; }
    { const char * tb_v1464 = "static inline _Bool tb_bpop(tb_list *l, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1464; }
    { const char * tb_v1465 = "    if (l->len == 0) tb_crash(line, \".pop() у пустого списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1465; }
    { const char * tb_v1466 = "    l->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1466; }
    { const char * tb_v1467 = "    return TB_BIT(l, l->len);\n"; *(const char * *)tb_grow(v_lines) = tb_v1467; }
    { const char * tb_v1468 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1468; }
    { const char * tb_v1469 = "// N одинаковых битов в конец (repeat N times: flags.add(true))\n"; *(const char * *)tb_grow(v_lines) = tb_v1469; }
    { const char * tb_v1470 = "static void tb_bfill(tb_list *l, long long n, _Bool v) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1470; }
    { const char * tb_v1471 = "    if (n <= 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1471; }
    { const char * tb_v1472 = "    if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1472; }
    { const char * tb_v1473 = "    long long need = l->len + n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1473; }
    { const char * tb_v1474 = "    if (need > l->cap) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1474; }
    { const char * tb_v1475 = "        long long old = l->cap / 8, cap = l->cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1475; }
    { const char * tb_v1476 = "        while (cap < need) cap *= 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1476; }
    { const char * tb_v1477 = "        l->data = tb_resize(l->data, old, cap / 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1477; }
    { const char * tb_v1478 = "        memset(l->data + old, 0, cap / 8 - old);\n"; *(const char * *)tb_grow(v_lines) = tb_v1478; }
    { const char * tb_v1479 = "        l->cap = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1479; }
    { const char * tb_v1480 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1480; }
    { const char * tb_v1481 = "    unsigned long long *w = (unsigned long long *)l->data;\n"; *(const char * *)tb_grow(v_lines) = tb_v1481; }
    { const char * tb_v1482 = "    long long i = l->len;\n"; *(const char * *)tb_grow(v_lines) = tb_v1482; }
    { const char * tb_v1483 = "    for (; i < need && (i & 63); i++) w[i >> 6] = v ? (w[i >> 6] | 1ULL << (i & 63)) : (w[i >> 6] & ~(1ULL << (i & 63)));\n"; *(const char * *)tb_grow(v_lines) = tb_v1483; }
    { const char * tb_v1484 = "    if (need - i >= 64) { memset(w + (i >> 6), v ? 0xff : 0, ((need - i) >> 6) * 8); i += (need - i) & ~63LL; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1484; }
    { const char * tb_v1485 = "    for (; i < need; i++) w[i >> 6] = v ? (w[i >> 6] | 1ULL << (i & 63)) : (w[i >> 6] & ~(1ULL << (i & 63)));\n"; *(const char * *)tb_grow(v_lines) = tb_v1485; }
    { const char * tb_v1486 = "    l->len = need;\n"; *(const char * *)tb_grow(v_lines) = tb_v1486; }
    { const char * tb_v1487 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1487; }
    { const char * tb_v1488 = "// значения dict bool лежат байтами — в list bool их надо переложить в биты\n"; *(const char * *)tb_grow(v_lines) = tb_v1488; }
    { const char * tb_v1489 = "static tb_list *tb_bits_of_bytes(tb_list *b) { return tb_bits_from(b->len, (const unsigned char *)b->data); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1489; }
    { const char * tb_v1490 = "// N одинаковых элементов в конец (repeat N times: nums.add(0))\n"; *(const char * *)tb_grow(v_lines) = tb_v1490; }
    { const char * tb_v1491 = "static void tb_fill(tb_list *l, long long n, const void *v) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1491; }
    { const char * tb_v1492 = "    if (n <= 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1492; }
    { const char * tb_v1493 = "    if (l->len + n > l->cap) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1493; }
    { const char * tb_v1494 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1494; }
    { const char * tb_v1495 = "        long long cap = l->cap * 2 + 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v1495; }
    { const char * tb_v1496 = "        if (cap < l->len + n) cap = l->len + n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1496; }
    { const char * tb_v1497 = "        l->data = tb_resize(l->data, l->cap * l->size, cap * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1497; }
    { const char * tb_v1498 = "        l->cap = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1498; }
    { const char * tb_v1499 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1499; }
    { const char * tb_v1500 = "    char *p = l->data + l->len * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1500; }
    { const char * tb_v1501 = "    int zero = 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1501; }
    { const char * tb_v1502 = "    for (long long b = 0; b < l->size; b++) if (((const char *)v)[b]) zero = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1502; }
    { const char * tb_v1503 = "    if (zero) memset(p, 0, n * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1503; }
    { const char * tb_v1504 = "    else for (long long i = 0; i < n; i++) memcpy(p + i * l->size, v, l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1504; }
    { const char * tb_v1505 = "    l->len += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1505; }
    { const char * tb_v1506 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1506; }
    { const char * tb_v1507 = "static void *tb_nn(void *p, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1507; }
    { const char * tb_v1508 = "    if (p == 0) tb_crash(line, \"obj пустой: его ещё не создали\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1508; }
    { const char * tb_v1509 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1509; }
    { const char * tb_v1510 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1510; }
    { const char * tb_v1511 = "static const char *tb_cat(const char *a, const char *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1511; }
    { const char * tb_v1512 = "    size_t la = strlen(a), lb = strlen(b);\n"; *(const char * *)tb_grow(v_lines) = tb_v1512; }
    { const char * tb_v1513 = "    char *s = tb_mem(la + lb + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1513; }
    { const char * tb_v1514 = "    memcpy(s, a, la);\n"; *(const char * *)tb_grow(v_lines) = tb_v1514; }
    { const char * tb_v1515 = "    memcpy(s + la, b, lb + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1515; }
    { const char * tb_v1516 = "    return s;\n"; *(const char * *)tb_grow(v_lines) = tb_v1516; }
    { const char * tb_v1517 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1517; }
    { const char * tb_v1518 = "static const char *tb_itos(long long x) { char *s = tb_mem(32); snprintf(s, 32, \"%lld\", x); return s; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1518; }
    { const char * tb_v1519 = "static const char *tb_dtos(double x) { char *s = tb_mem(40); snprintf(s, 40, \"%g\", x); return s; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1519; }
    { const char * tb_v1520 = "static const char *tb_btos(int x) { return x ? \"true\" : \"false\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1520; }
    { const char * tb_v1521 = "// текст из одного байта: 256 готовых строк, без выделения памяти на каждую букву\n"; *(const char * *)tb_grow(v_lines) = tb_v1521; }
    { const char * tb_v1522 = "// таблица постоянная: компилятор C видит, что второй байт всегда 0, и  ch == \",\"  становится одним сравнением байта\n"; *(const char * *)tb_grow(v_lines) = tb_v1522; }
    { const char * tb_v1523 = "static const char tb_chars[512] = {\n"; *(const char * *)tb_grow(v_lines) = tb_v1523; }
    { const char * tb_v1524 = "    0,0,1,0,2,0,3,0,4,0,5,0,6,0,7,0,8,0,9,0,10,0,11,0,12,0,13,0,14,0,15,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1524; }
    { const char * tb_v1525 = "    16,0,17,0,18,0,19,0,20,0,21,0,22,0,23,0,24,0,25,0,26,0,27,0,28,0,29,0,30,0,31,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1525; }
    { const char * tb_v1526 = "    32,0,33,0,34,0,35,0,36,0,37,0,38,0,39,0,40,0,41,0,42,0,43,0,44,0,45,0,46,0,47,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1526; }
    { const char * tb_v1527 = "    48,0,49,0,50,0,51,0,52,0,53,0,54,0,55,0,56,0,57,0,58,0,59,0,60,0,61,0,62,0,63,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1527; }
    { const char * tb_v1528 = "    64,0,65,0,66,0,67,0,68,0,69,0,70,0,71,0,72,0,73,0,74,0,75,0,76,0,77,0,78,0,79,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1528; }
    { const char * tb_v1529 = "    80,0,81,0,82,0,83,0,84,0,85,0,86,0,87,0,88,0,89,0,90,0,91,0,92,0,93,0,94,0,95,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1529; }
    { const char * tb_v1530 = "    96,0,97,0,98,0,99,0,100,0,101,0,102,0,103,0,104,0,105,0,106,0,107,0,108,0,109,0,110,0,111,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1530; }
    { const char * tb_v1531 = "    112,0,113,0,114,0,115,0,116,0,117,0,118,0,119,0,120,0,121,0,122,0,123,0,124,0,125,0,126,0,127,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1531; }
    { const char * tb_v1532 = "    128,0,129,0,130,0,131,0,132,0,133,0,134,0,135,0,136,0,137,0,138,0,139,0,140,0,141,0,142,0,143,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1532; }
    { const char * tb_v1533 = "    144,0,145,0,146,0,147,0,148,0,149,0,150,0,151,0,152,0,153,0,154,0,155,0,156,0,157,0,158,0,159,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1533; }
    { const char * tb_v1534 = "    160,0,161,0,162,0,163,0,164,0,165,0,166,0,167,0,168,0,169,0,170,0,171,0,172,0,173,0,174,0,175,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1534; }
    { const char * tb_v1535 = "    176,0,177,0,178,0,179,0,180,0,181,0,182,0,183,0,184,0,185,0,186,0,187,0,188,0,189,0,190,0,191,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1535; }
    { const char * tb_v1536 = "    192,0,193,0,194,0,195,0,196,0,197,0,198,0,199,0,200,0,201,0,202,0,203,0,204,0,205,0,206,0,207,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1536; }
    { const char * tb_v1537 = "    208,0,209,0,210,0,211,0,212,0,213,0,214,0,215,0,216,0,217,0,218,0,219,0,220,0,221,0,222,0,223,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1537; }
    { const char * tb_v1538 = "    224,0,225,0,226,0,227,0,228,0,229,0,230,0,231,0,232,0,233,0,234,0,235,0,236,0,237,0,238,0,239,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1538; }
    { const char * tb_v1539 = "    240,0,241,0,242,0,243,0,244,0,245,0,246,0,247,0,248,0,249,0,250,0,251,0,252,0,253,0,254,0,255,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1539; }
    { const char * tb_v1540 = "};\n"; *(const char * *)tb_grow(v_lines) = tb_v1540; }
    { const char * tb_v1541 = "static inline const char *tb_chr(long long c) { return tb_chars + 2 * (unsigned char)c; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1541; }
    { const char * tb_v1542 = "static inline long long tb_tlen(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1542; }
    { const char * tb_v1543 = "    if (tb_lc_p[0] == s) return tb_lc_n[0];\n"; *(const char * *)tb_grow(v_lines) = tb_v1543; }
    { const char * tb_v1544 = "    if (tb_lc_p[1] == s) return tb_lc_n[1];\n"; *(const char * *)tb_grow(v_lines) = tb_v1544; }
    { const char * tb_v1545 = "    long long n = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1545; }
    { const char * tb_v1546 = "    tb_lc_p[tb_lc_next] = s; tb_lc_n[tb_lc_next] = n; tb_lc_next ^= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1546; }
    { const char * tb_v1547 = "    return n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1547; }
    { const char * tb_v1548 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1548; }
    { const char * tb_v1549 = "// n — длина, если компилятор её знает (текст, в который дописывают), иначе -1\n"; *(const char * *)tb_grow(v_lines) = tb_v1549; }
    { const char * tb_v1550 = "static inline const char *tb_char_at(const char *s, long long i, long long n, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1550; }
    { const char * tb_v1551 = "    if (n < 0) n = tb_tlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1551; }
    { const char * tb_v1552 = "    if (i < 0 || i >= n) tb_crash(line, \"номер за пределами текста\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1552; }
    { const char * tb_v1553 = "    return tb_chr((unsigned char)s[i]);\n"; *(const char * *)tb_grow(v_lines) = tb_v1553; }
    { const char * tb_v1554 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1554; }
    { const char * tb_v1555 = "// s += b: дописать в конец своей памяти s, а не собирать третий текст из двух.\n"; *(const char * *)tb_grow(v_lines) = tb_v1555; }
    { const char * tb_v1556 = "// *rl — длина s в своей памяти (-1: своей памяти нет, s могли сохранить где-то ещё — сначала копия),\n"; *(const char * *)tb_grow(v_lines) = tb_v1556; }
    { const char * tb_v1557 = "// *rc — сколько там места.\n"; *(const char * *)tb_grow(v_lines) = tb_v1557; }
    { const char * tb_v1558 = "static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1558; }
    { const char * tb_v1559 = "// число цифрами в конец буфера d[24]: вернёт начало цифр, длину — в *n\n"; *(const char * *)tb_grow(v_lines) = tb_v1559; }
    { const char * tb_v1560 = "static inline const char *tb_digits(long long x, char *d, long long *n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1560; }
    { const char * tb_v1561 = "    unsigned long long u = x < 0 ? 0ULL - (unsigned long long)x : (unsigned long long)x;\n"; *(const char * *)tb_grow(v_lines) = tb_v1561; }
    { const char * tb_v1562 = "    char *p = d + 24;\n"; *(const char * *)tb_grow(v_lines) = tb_v1562; }
    { const char * tb_v1563 = "    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);\n"; *(const char * *)tb_grow(v_lines) = tb_v1563; }
    { const char * tb_v1564 = "    if (x < 0) *--p = '-';\n"; *(const char * *)tb_grow(v_lines) = tb_v1564; }
    { const char * tb_v1565 = "    *n = d + 24 - p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1565; }
    { const char * tb_v1566 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1566; }
    { const char * tb_v1567 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1567; }
    { const char * tb_v1568 = "static inline void tb_app(const char **p, long long *rl, long long *rc, const char *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1568; }
    { const char * tb_v1569 = "    tb_app_n(p, rl, rc, b, (long long)strlen(b));\n"; *(const char * *)tb_grow(v_lines) = tb_v1569; }
    { const char * tb_v1570 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1570; }
    { const char * tb_v1571 = "// дописать число: цифры пишутся прямо в конец, без отдельного текста\n"; *(const char * *)tb_grow(v_lines) = tb_v1571; }
    { const char * tb_v1572 = "static inline void tb_app_i(const char **p, long long *rl, long long *rc, long long x) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1572; }
    { const char * tb_v1573 = "    char d[24]; int n = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1573; }
    { const char * tb_v1574 = "    unsigned long long u = x < 0 ? 0ULL - (unsigned long long)x : (unsigned long long)x;\n"; *(const char * *)tb_grow(v_lines) = tb_v1574; }
    { const char * tb_v1575 = "    do { d[23 - n++] = (char)('0' + u % 10); u /= 10; } while (u);\n"; *(const char * *)tb_grow(v_lines) = tb_v1575; }
    { const char * tb_v1576 = "    if (x < 0) d[23 - n++] = '-';\n"; *(const char * *)tb_grow(v_lines) = tb_v1576; }
    { const char * tb_v1577 = "    tb_app_n(p, rl, rc, d + 24 - n, n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1577; }
    { const char * tb_v1578 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1578; }
    { const char * tb_v1579 = "static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1579; }
    { const char * tb_v1580 = "// ядро отдаёт свой кусок в общий текст: если общий ещё пустой — забирает память куска целиком, без копии\n"; *(const char * *)tb_grow(v_lines) = tb_v1580; }
    { const char * tb_v1581 = "static void tb_app_take(const char **p, long long *rl, long long *rc, const char *b, long long brl, long long brc) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1581; }
    { const char * tb_v1582 = "    if (*rl < 0 && (*p)[0] == 0 && brl >= 0) { *p = b; *rl = brl; *rc = brc; return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1582; }
    { const char * tb_v1583 = "    tb_app_n(p, rl, rc, b, brl >= 0 ? brl : (long long)strlen(b));\n"; *(const char * *)tb_grow(v_lines) = tb_v1583; }
    { const char * tb_v1584 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1584; }
    { const char * tb_v1585 = "static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1585; }
    { const char * tb_v1586 = "    char *s = (char *)*p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1586; }
    { const char * tb_v1587 = "    if (*rl < 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1587; }
    { const char * tb_v1588 = "        long long la = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1588; }
    { const char * tb_v1589 = "        long long cap = 2 * (la + lb) + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1589; }
    { const char * tb_v1590 = "        if (cap < 64) cap = 64;\n"; *(const char * *)tb_grow(v_lines) = tb_v1590; }
    { const char * tb_v1591 = "        char *r = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1591; }
    { const char * tb_v1592 = "        memcpy(r, s, la);\n"; *(const char * *)tb_grow(v_lines) = tb_v1592; }
    { const char * tb_v1593 = "        memcpy(r + la, b, lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1593; }
    { const char * tb_v1594 = "        r[la + lb] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1594; }
    { const char * tb_v1595 = "        *p = r; *rl = la + lb; *rc = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1595; }
    { const char * tb_v1596 = "        return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1596; }
    { const char * tb_v1597 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1597; }
    { const char * tb_v1598 = "    long long need = *rl + lb + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1598; }
    { const char * tb_v1599 = "    if (need > *rc) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1599; }
    { const char * tb_v1600 = "        long long cap = 2 * need;\n"; *(const char * *)tb_grow(v_lines) = tb_v1600; }
    { const char * tb_v1601 = "        long long off = (b >= s && b < s + *rc) ? b - s : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1601; }
    { const char * tb_v1602 = "        s = tb_resize(s, *rl + 1, cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1602; }
    { const char * tb_v1603 = "        if (off >= 0) b = s + off;\n"; *(const char * *)tb_grow(v_lines) = tb_v1603; }
    { const char * tb_v1604 = "        *p = s; *rc = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1604; }
    { const char * tb_v1605 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1605; }
    { const char * tb_v1606 = "    memmove(s + *rl, b, lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1606; }
    { const char * tb_v1607 = "    *rl += lb;\n"; *(const char * *)tb_grow(v_lines) = tb_v1607; }
    { const char * tb_v1608 = "    s[*rl] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1608; }
    { const char * tb_v1609 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1609; }
    { const char * tb_v1610 = "static const char *tb_cut(const char *s, long long from, long long to, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1610; }
    { const char * tb_v1611 = "    long long n = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1611; }
    { const char * tb_v1612 = "    if (from < 0 || to >= n || from > to + 1) tb_crash(line, \"cut: номера за пределами текста\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1612; }
    { const char * tb_v1613 = "    char *r = tb_mem(to - from + 2);\n"; *(const char * *)tb_grow(v_lines) = tb_v1613; }
    { const char * tb_v1614 = "    memcpy(r, s + from, to - from + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1614; }
    { const char * tb_v1615 = "    r[to - from + 1] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1615; }
    { const char * tb_v1616 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1616; }
    { const char * tb_v1617 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1617; }
    { const char * tb_v1618 = "static long long tb_code(const char *s, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1618; }
    { const char * tb_v1619 = "    if (s[0] == 0) tb_crash(line, \"code: текст пустой\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1619; }
    { const char * tb_v1620 = "    return (unsigned char)s[0];\n"; *(const char * *)tb_grow(v_lines) = tb_v1620; }
    { const char * tb_v1621 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1621; }
    { const char * tb_v1622 = "static long long tb_div(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"деление на ноль\"); return a / b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1622; }
    { const char * tb_v1623 = "static long long tb_mod(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"остаток от деления на ноль\"); return a % b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1623; }
    { const char * tb_v1624 = "static long long tb_round(double x) { return (long long)(x < 0 ? x - 0.5 : x + 0.5); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1624; }
    { const char * tb_v1625 = "static void tb_ps(const char *s) { fputs(s, stdout); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1625; }
    { const char * tb_v1626 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1626; }
    { const char * tb_v1627 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1627; }
    { const char * tb_v1628 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1628; }
    { const char * tb_v1629 = "// ---------- ошибки fail / try / or / else err ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1629; }
    { const char * tb_v1630 = "// Функция, которая сломалась, кладёт текст ошибки в tb_err и сразу выходит.\n"; *(const char * *)tb_grow(v_lines) = tb_v1630; }
    { const char * tb_v1631 = "// Тот, кто её вызвал, смотрит в tb_err.\n"; *(const char * *)tb_grow(v_lines) = tb_v1631; }
    { const char * tb_v1632 = "static _Thread_local const char *tb_err = 0;   // у каждого ядра своя\n"; *(const char * *)tb_grow(v_lines) = tb_v1632; }
    { const char * tb_v1633 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1633; }
    { const char * tb_v1634 = "TB_COLD static void tb_die(const char *msg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1634; }
    { const char * tb_v1635 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v1635; }
    { const char * tb_v1636 = "    fprintf(stderr, \"ошибка: %s\\n\", msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v1636; }
    { const char * tb_v1637 = "    exit(1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1637; }
    { const char * tb_v1638 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1638; }
    { const char * tb_v1639 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1639; }
    { const char * tb_v1640 = "// ---------- текст ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1640; }
    { const char * tb_v1641 = "static const char *tb_upper(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1641; }
    { const char * tb_v1642 = "    size_t n = strlen(s); char *r = tb_mem(n + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1642; }
    { const char * tb_v1643 = "    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v1643; }
    { const char * tb_v1644 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1644; }
    { const char * tb_v1645 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1645; }
    { const char * tb_v1646 = "static const char *tb_lower(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1646; }
    { const char * tb_v1647 = "    size_t n = strlen(s); char *r = tb_mem(n + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1647; }
    { const char * tb_v1648 = "    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v1648; }
    { const char * tb_v1649 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1649; }
    { const char * tb_v1650 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1650; }
    { const char * tb_v1651 = "static int tb_space(char c) { return c == ' ' || c == '\\t' || c == '\\n' || c == '\\r'; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1651; }
    { const char * tb_v1652 = "static const char *tb_trim(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1652; }
    { const char * tb_v1653 = "    size_t a = 0, b = strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1653; }
    { const char * tb_v1654 = "    while (a < b && tb_space(s[a])) a++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1654; }
    { const char * tb_v1655 = "    while (b > a && tb_space(s[b - 1])) b--;\n"; *(const char * *)tb_grow(v_lines) = tb_v1655; }
    { const char * tb_v1656 = "    char *r = tb_mem(b - a + 1); memcpy(r, s + a, b - a); r[b - a] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1656; }
    { const char * tb_v1657 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1657; }
    { const char * tb_v1658 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1658; }
    { const char * tb_v1659 = "static long long tb_find(const char *s, const char *part) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1659; }
    { const char * tb_v1660 = "    const char *p = strstr(s, part);\n"; *(const char * *)tb_grow(v_lines) = tb_v1660; }
    { const char * tb_v1661 = "    return p ? (long long)(p - s) : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1661; }
    { const char * tb_v1662 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1662; }
    { const char * tb_v1663 = "static int tb_starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1663; }
    { const char * tb_v1664 = "static int tb_ends(const char *s, const char *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1664; }
    { const char * tb_v1665 = "    size_t a = strlen(s), b = strlen(p);\n"; *(const char * *)tb_grow(v_lines) = tb_v1665; }
    { const char * tb_v1666 = "    return b <= a && strcmp(s + a - b, p) == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1666; }
    { const char * tb_v1667 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1667; }
    { const char * tb_v1668 = "static const char *tb_replace(const char *s, const char *from, const char *to, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1668; }
    { const char * tb_v1669 = "    size_t lf = strlen(from), lt = strlen(to), count = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1669; }
    { const char * tb_v1670 = "    if (lf == 0) tb_crash(line, \"replace: что заменять — пустой текст\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1670; }
    { const char * tb_v1671 = "    for (const char *p = strstr(s, from); p; p = strstr(p + lf, from)) count++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1671; }
    { const char * tb_v1672 = "    char *r = tb_mem(strlen(s) + count * lt + 1), *w = r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1672; }
    { const char * tb_v1673 = "    const char *p = s, *q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1673; }
    { const char * tb_v1674 = "    while ((q = strstr(p, from)) != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1674; }
    { const char * tb_v1675 = "        memcpy(w, p, q - p); w += q - p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1675; }
    { const char * tb_v1676 = "        memcpy(w, to, lt); w += lt;\n"; *(const char * *)tb_grow(v_lines) = tb_v1676; }
    { const char * tb_v1677 = "        p = q + lf;\n"; *(const char * *)tb_grow(v_lines) = tb_v1677; }
    { const char * tb_v1678 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1678; }
    { const char * tb_v1679 = "    strcpy(w, p);\n"; *(const char * *)tb_grow(v_lines) = tb_v1679; }
    { const char * tb_v1680 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1680; }
    { const char * tb_v1681 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1681; }
    { const char * tb_v1682 = "static tb_list *tb_split(const char *s, const char *sep, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1682; }
    { const char * tb_v1683 = "    size_t ls = strlen(sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v1683; }
    { const char * tb_v1684 = "    if (ls == 0) tb_crash(line, \"split: разделитель — пустой текст\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1684; }
    { const char * tb_v1685 = "    tb_list *l = tb_list_new(sizeof(const char *));\n"; *(const char * *)tb_grow(v_lines) = tb_v1685; }
    { const char * tb_v1686 = "    const char *p = s, *q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1686; }
    { const char * tb_v1687 = "    while (1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1687; }
    { const char * tb_v1688 = "        q = strstr(p, sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v1688; }
    { const char * tb_v1689 = "        size_t n = q ? (size_t)(q - p) : strlen(p);\n"; *(const char * *)tb_grow(v_lines) = tb_v1689; }
    { const char * tb_v1690 = "        char *part = tb_mem(n + 1); memcpy(part, p, n); part[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1690; }
    { const char * tb_v1691 = "        *(const char **)tb_grow(l) = part;\n"; *(const char * *)tb_grow(v_lines) = tb_v1691; }
    { const char * tb_v1692 = "        if (!q) break;\n"; *(const char * *)tb_grow(v_lines) = tb_v1692; }
    { const char * tb_v1693 = "        p = q + ls;\n"; *(const char * *)tb_grow(v_lines) = tb_v1693; }
    { const char * tb_v1694 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1694; }
    { const char * tb_v1695 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1695; }
    { const char * tb_v1696 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1696; }
    { const char * tb_v1697 = "static const char *tb_join(tb_list *l, const char *sep) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1697; }
    { const char * tb_v1698 = "    size_t n = 0, ls = strlen(sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v1698; }
    { const char * tb_v1699 = "    for (long long i = 0; i < l->len; i++) n += strlen(((const char **)l->data)[i]) + ls;\n"; *(const char * *)tb_grow(v_lines) = tb_v1699; }
    { const char * tb_v1700 = "    char *r = tb_mem(n + 1), *w = r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1700; }
    { const char * tb_v1701 = "    for (long long i = 0; i < l->len; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1701; }
    { const char * tb_v1702 = "        const char *part = ((const char **)l->data)[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v1702; }
    { const char * tb_v1703 = "        if (i > 0) { memcpy(w, sep, ls); w += ls; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1703; }
    { const char * tb_v1704 = "        memcpy(w, part, strlen(part)); w += strlen(part);\n"; *(const char * *)tb_grow(v_lines) = tb_v1704; }
    { const char * tb_v1705 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1705; }
    { const char * tb_v1706 = "    *w = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1706; }
    { const char * tb_v1707 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1707; }
    { const char * tb_v1708 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1708; }
    { const char * tb_v1709 = "static long long tb_to_int(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1709; }
    { const char * tb_v1710 = "    long long v = 0; int i = 0, minus = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1710; }
    { const char * tb_v1711 = "    if (s[0] == '-') { minus = 1; i = 1; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1711; }
    { const char * tb_v1712 = "    if (s[i] == 0) { tb_err = tb_cat(\"это не число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1712; }
    { const char * tb_v1713 = "    for (; s[i]; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1713; }
    { const char * tb_v1714 = "        if (s[i] < '0' || s[i] > '9') { tb_err = tb_cat(\"это не число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1714; }
    { const char * tb_v1715 = "        v = v * 10 + (s[i] - '0');\n"; *(const char * *)tb_grow(v_lines) = tb_v1715; }
    { const char * tb_v1716 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1716; }
    { const char * tb_v1717 = "    return minus ? -v : v;\n"; *(const char * *)tb_grow(v_lines) = tb_v1717; }
    { const char * tb_v1718 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1718; }
    { const char * tb_v1719 = "static double tb_to_dec(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1719; }
    { const char * tb_v1720 = "    char *end = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1720; }
    { const char * tb_v1721 = "    double v = strtod(s, &end);\n"; *(const char * *)tb_grow(v_lines) = tb_v1721; }
    { const char * tb_v1722 = "    if (s[0] == 0 || *end != 0) { tb_err = tb_cat(\"это не дробное число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1722; }
    { const char * tb_v1723 = "    return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v1723; }
    { const char * tb_v1724 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1724; }
    { const char * tb_v1725 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1725; }
    { const char * tb_v1726 = "// ---------- файлы, ввод, слова запуска ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1726; }
    { const char * tb_v1727 = "static const char *tb_read_file(const char *path) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1727; }
    { const char * tb_v1728 = "    FILE *f = fopen(path, \"rb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1728; }
    { const char * tb_v1729 = "    if (f == 0) { tb_err = tb_cat(\"не могу открыть файл: \", path); return \"\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1729; }
    { const char * tb_v1730 = "    size_t cap = 4096, n = 0, got;\n"; *(const char * *)tb_grow(v_lines) = tb_v1730; }
    { const char * tb_v1731 = "    char *buf = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1731; }
    { const char * tb_v1732 = "    while ((got = fread(buf + n, 1, cap - n - 1, f)) > 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1732; }
    { const char * tb_v1733 = "        n += got;\n"; *(const char * *)tb_grow(v_lines) = tb_v1733; }
    { const char * tb_v1734 = "        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1734; }
    { const char * tb_v1735 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1735; }
    { const char * tb_v1736 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1736; }
    { const char * tb_v1737 = "    buf[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1737; }
    { const char * tb_v1738 = "    return buf;\n"; *(const char * *)tb_grow(v_lines) = tb_v1738; }
    { const char * tb_v1739 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1739; }
    { const char * tb_v1740 = "static void tb_write_file(const char *path, const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1740; }
    { const char * tb_v1741 = "    FILE *f = fopen(path, \"wb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1741; }
    { const char * tb_v1742 = "    if (f == 0) { tb_err = tb_cat(\"не могу записать файл: \", path); return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1742; }
    { const char * tb_v1743 = "    fputs(s, f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1743; }
    { const char * tb_v1744 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1744; }
    { const char * tb_v1745 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1745; }
    { const char * tb_v1746 = "static const char *tb_input(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1746; }
    { const char * tb_v1747 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v1747; }
    { const char * tb_v1748 = "    size_t cap = 256, n = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1748; }
    { const char * tb_v1749 = "    char *buf = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1749; }
    { const char * tb_v1750 = "    int c;\n"; *(const char * *)tb_grow(v_lines) = tb_v1750; }
    { const char * tb_v1751 = "    while ((c = getchar()) != EOF && c != '\\n') {\n"; *(const char * *)tb_grow(v_lines) = tb_v1751; }
    { const char * tb_v1752 = "        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1752; }
    { const char * tb_v1753 = "        buf[n++] = (char)c;\n"; *(const char * *)tb_grow(v_lines) = tb_v1753; }
    { const char * tb_v1754 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1754; }
    { const char * tb_v1755 = "    if (c == EOF && n == 0) { tb_err = \"ввод кончился\"; return \"\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1755; }
    { const char * tb_v1756 = "    if (n > 0 && buf[n - 1] == '\\r') n--;\n"; *(const char * *)tb_grow(v_lines) = tb_v1756; }
    { const char * tb_v1757 = "    buf[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1757; }
    { const char * tb_v1758 = "    return buf;\n"; *(const char * *)tb_grow(v_lines) = tb_v1758; }
    { const char * tb_v1759 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1759; }
    { const char * tb_v1760 = "static tb_list *tb_args;\n"; *(const char * *)tb_grow(v_lines) = tb_v1760; }
    { const char * tb_v1761 = "static long long tb_run(const char *command) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1761; }
    { const char * tb_v1762 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v1762; }
    { const char * tb_v1763 = "    int code = system(command);\n"; *(const char * *)tb_grow(v_lines) = tb_v1763; }
    { const char * tb_v1764 = "    if (code == -1) return -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1764; }
    { const char * tb_v1765 = "    return code == 0 ? 0 : (code > 255 ? code / 256 : code);\n"; *(const char * *)tb_grow(v_lines) = tb_v1765; }
    { const char * tb_v1766 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1766; }
    { const char * tb_v1767 = "static void tb_exit(long long code) { fflush(stdout); exit((int)code); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1767; }
    { const char * tb_v1768 = "static int tb_file_exists(const char *path) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1768; }
    { const char * tb_v1769 = "    FILE *f = fopen(path, \"rb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1769; }
    { const char * tb_v1770 = "    if (f == 0) return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1770; }
    { const char * tb_v1771 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1771; }
    { const char * tb_v1772 = "    return 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1772; }
    { const char * tb_v1773 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1773; }
    { const char * tb_v1774 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1774; }
    { const char * tb_v1775 = "// ---------- числа ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1775; }
    { const char * tb_v1776 = "static long long tb_absi(long long x) { return x < 0 ? -x : x; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1776; }
    { const char * tb_v1777 = "static long long tb_mini(long long a, long long b) { return a < b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1777; }
    { const char * tb_v1778 = "static long long tb_maxi(long long a, long long b) { return a > b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1778; }
    { const char * tb_v1779 = "static double tb_mind(double a, double b) { return a < b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1779; }
    { const char * tb_v1780 = "static double tb_maxd(double a, double b) { return a > b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1780; }
    { const char * tb_v1781 = "static double tb_sqrt(double x, int line) { if (x < 0) tb_crash(line, \"sqrt: корень из отрицательного числа\"); return sqrt(x); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1781; }
    { const char * tb_v1782 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1782; }
    { const char * tb_v1783 = "// ---------- sort ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1783; }
    { const char * tb_v1784 = "static int tb_cmp_int(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return (x > y) - (x < y); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1784; }
    { const char * tb_v1785 = "static int tb_cmp_dec(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1785; }
    { const char * tb_v1786 = "static int tb_cmp_text(const void *a, const void *b) { return strcmp(*(const char * const *)a, *(const char * const *)b); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1786; }
    { const char * tb_v1787 = "static tb_list *tb_copy(tb_list *l) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1787; }
    { const char * tb_v1788 = "    if (l->size == 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1788; }
    { const char * tb_v1789 = "        tb_list *r = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1789; }
    { const char * tb_v1790 = "        long long bytes = l->cap / 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1790; }
    { const char * tb_v1791 = "        r->len = l->len; r->cap = l->cap; r->size = 0; r->data = tb_mem(bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v1791; }
    { const char * tb_v1792 = "        memcpy(r->data, l->data, bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v1792; }
    { const char * tb_v1793 = "        return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1793; }
    { const char * tb_v1794 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1794; }
    { const char * tb_v1795 = "    return tb_list_from(l->len, l->size, l->data);\n"; *(const char * *)tb_grow(v_lines) = tb_v1795; }
    { const char * tb_v1796 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1796; }
    { const char * tb_v1797 = "static void tb_sort_list(tb_list *l, int (*cmp)(const void *, const void *)) { qsort(l->data, l->len, l->size, cmp); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1797; }
    { const char * tb_v1798 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1798; }
    { const char * tb_v1799 = "// ---------- dict: ключи и значения лежат в двух списках по порядку добавления,\n"; *(const char * *)tb_grow(v_lines) = tb_v1799; }
    { const char * tb_v1800 = "// а найти номер по ключу можно тремя способами (mode):\n"; *(const char * *)tb_grow(v_lines) = tb_v1800; }
    { const char * tb_v1801 = "//   1 — «номер = адрес»: ключи int шли подряд base, base+1, base+2 ... — номер = ключ - base, без поиска;\n"; *(const char * *)tb_grow(v_lines) = tb_v1801; }
    { const char * tb_v1802 = "//   2 — «адресное хранение»: ключи int лежат плотно — pos[ключ - base] = номер + 1, один взгляд;\n"; *(const char * *)tb_grow(v_lines) = tb_v1802; }
    { const char * tb_v1803 = "//   0 — таблица slots с хешем (текст или разбросанные числа) ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1803; }
    { const char * tb_v1804 = "typedef struct {\n"; *(const char * *)tb_grow(v_lines) = tb_v1804; }
    { const char * tb_v1805 = "    tb_list *keys; tb_list *vals; long long *slots; long long nslots; int text;\n"; *(const char * *)tb_grow(v_lines) = tb_v1805; }
    { const char * tb_v1806 = "    int mode; long long base; long long *pos; long long npos;\n"; *(const char * *)tb_grow(v_lines) = tb_v1806; }
    { const char * tb_v1807 = "} tb_dict;\n"; *(const char * *)tb_grow(v_lines) = tb_v1807; }
    { const char * tb_v1808 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1808; }
    { const char * tb_v1809 = "static unsigned long long tb_hash(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1809; }
    { const char * tb_v1810 = "    unsigned long long h = 1469598103934665603ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v1810; }
    { const char * tb_v1811 = "    if (d->text) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1811; }
    { const char * tb_v1812 = "        for (const unsigned char *p = *(const unsigned char * const *)k; *p; p++) { h ^= *p; h *= 1099511628211ULL; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1812; }
    { const char * tb_v1813 = "    } else {\n"; *(const char * *)tb_grow(v_lines) = tb_v1813; }
    { const char * tb_v1814 = "        h ^= (unsigned long long)*(const long long *)k;\n"; *(const char * *)tb_grow(v_lines) = tb_v1814; }
    { const char * tb_v1815 = "        h *= 0x9E3779B97F4A7C15ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v1815; }
    { const char * tb_v1816 = "        h ^= h >> 32;\n"; *(const char * *)tb_grow(v_lines) = tb_v1816; }
    { const char * tb_v1817 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1817; }
    { const char * tb_v1818 = "    return h;\n"; *(const char * *)tb_grow(v_lines) = tb_v1818; }
    { const char * tb_v1819 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1819; }
    { const char * tb_v1820 = "static int tb_keq(tb_dict *d, const void *a, const void *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1820; }
    { const char * tb_v1821 = "    if (d->text) return strcmp(*(const char * const *)a, *(const char * const *)b) == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1821; }
    { const char * tb_v1822 = "    return *(const long long *)a == *(const long long *)b;\n"; *(const char * *)tb_grow(v_lines) = tb_v1822; }
    { const char * tb_v1823 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1823; }
    { const char * tb_v1824 = "static inline long long tb_dfind(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1824; }
    { const char * tb_v1825 = "    if (d->mode == 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1825; }
    { const char * tb_v1826 = "        unsigned long long at = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v1826; }
    { const char * tb_v1827 = "        return at < (unsigned long long)d->keys->len ? (long long)at : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1827; }
    { const char * tb_v1828 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1828; }
    { const char * tb_v1829 = "    if (d->mode == 2) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1829; }
    { const char * tb_v1830 = "        unsigned long long o = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v1830; }
    { const char * tb_v1831 = "        return o < (unsigned long long)d->npos ? d->pos[o] - 1 : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1831; }
    { const char * tb_v1832 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1832; }
    { const char * tb_v1833 = "    unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1833; }
    { const char * tb_v1834 = "    while (d->slots[i] != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1834; }
    { const char * tb_v1835 = "        long long at = d->slots[i] - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1835; }
    { const char * tb_v1836 = "        if (tb_keq(d, d->keys->data + at * d->keys->size, k)) return at;\n"; *(const char * *)tb_grow(v_lines) = tb_v1836; }
    { const char * tb_v1837 = "        i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1837; }
    { const char * tb_v1838 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1838; }
    { const char * tb_v1839 = "    return -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1839; }
    { const char * tb_v1840 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1840; }
    { const char * tb_v1841 = "// память для таблиц dict — там же, где лежат его ключи (временная или обычная)\n"; *(const char * *)tb_grow(v_lines) = tb_v1841; }
    { const char * tb_v1842 = "static void *tb_dalloc(tb_dict *d, void *old, long long bytes) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1842; }
    { const char * tb_v1843 = "    int in_temp = tb_nchunks > 0 && tb_in_temp(d->keys->data);\n"; *(const char * *)tb_grow(v_lines) = tb_v1843; }
    { const char * tb_v1844 = "    if (old && !in_temp) free(old);\n"; *(const char * *)tb_grow(v_lines) = tb_v1844; }
    { const char * tb_v1845 = "    void *p = in_temp ? tb_temp_alloc(bytes) : malloc(bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v1845; }
    { const char * tb_v1846 = "    if (!p) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1846; }
    { const char * tb_v1847 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1847; }
    { const char * tb_v1848 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1848; }
    { const char * tb_v1849 = "static void tb_dreindex(tb_dict *d, long long nslots) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1849; }
    { const char * tb_v1850 = "    d->mode = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1850; }
    { const char * tb_v1851 = "    if (d->pos) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1851; }
    { const char * tb_v1852 = "        if (!(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);\n"; *(const char * *)tb_grow(v_lines) = tb_v1852; }
    { const char * tb_v1853 = "        d->pos = 0; d->npos = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1853; }
    { const char * tb_v1854 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1854; }
    { const char * tb_v1855 = "    d->nslots = nslots;\n"; *(const char * *)tb_grow(v_lines) = tb_v1855; }
    { const char * tb_v1856 = "    d->slots = tb_dalloc(d, d->slots, nslots * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1856; }
    { const char * tb_v1857 = "    memset(d->slots, 0, nslots * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1857; }
    { const char * tb_v1858 = "    unsigned long long mask = nslots - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1858; }
    { const char * tb_v1859 = "    for (long long at = 0; at < d->keys->len; at++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1859; }
    { const char * tb_v1860 = "        unsigned long long i = tb_hash(d, d->keys->data + at * d->keys->size) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1860; }
    { const char * tb_v1861 = "        while (d->slots[i] != 0) i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1861; }
    { const char * tb_v1862 = "        d->slots[i] = at + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1862; }
    { const char * tb_v1863 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1863; }
    { const char * tb_v1864 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1864; }
    { const char * tb_v1865 = "// перейти на таблицу с хешем (ключ ушёл далеко от остальных или его удалили)\n"; *(const char * *)tb_grow(v_lines) = tb_v1865; }
    { const char * tb_v1866 = "static void tb_dto_hash(tb_dict *d) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1866; }
    { const char * tb_v1867 = "    long long n = 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1867; }
    { const char * tb_v1868 = "    while (n < d->keys->len * 2 + 2) n *= 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1868; }
    { const char * tb_v1869 = "    tb_dreindex(d, n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1869; }
    { const char * tb_v1870 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1870; }
    { const char * tb_v1871 = "// mode 2: таблица pos на ключи base .. base+npos-1\n"; *(const char * *)tb_grow(v_lines) = tb_v1871; }
    { const char * tb_v1872 = "static void tb_dto_pos(tb_dict *d, long long npos) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1872; }
    { const char * tb_v1873 = "    long long *p = tb_dalloc(d, 0, npos * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1873; }
    { const char * tb_v1874 = "    memset(p, 0, npos * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1874; }
    { const char * tb_v1875 = "    const long long *ks = (const long long *)d->keys->data;\n"; *(const char * *)tb_grow(v_lines) = tb_v1875; }
    { const char * tb_v1876 = "    for (long long at = 0; at < d->keys->len; at++) p[ks[at] - d->base] = at + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1876; }
    { const char * tb_v1877 = "    if (d->pos && !(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);\n"; *(const char * *)tb_grow(v_lines) = tb_v1877; }
    { const char * tb_v1878 = "    d->pos = p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1878; }
    { const char * tb_v1879 = "    d->npos = npos;\n"; *(const char * *)tb_grow(v_lines) = tb_v1879; }
    { const char * tb_v1880 = "    d->mode = 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1880; }
    { const char * tb_v1881 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1881; }
    { const char * tb_v1882 = "static tb_dict *tb_dict_new(int text, long long vsize) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1882; }
    { const char * tb_v1883 = "    tb_dict *d = tb_mem(sizeof(tb_dict));\n"; *(const char * *)tb_grow(v_lines) = tb_v1883; }
    { const char * tb_v1884 = "    d->text = text;\n"; *(const char * *)tb_grow(v_lines) = tb_v1884; }
    { const char * tb_v1885 = "    d->keys = tb_list_new(text ? sizeof(const char *) : sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1885; }
    { const char * tb_v1886 = "    d->vals = tb_list_new(vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v1886; }
    { const char * tb_v1887 = "    d->slots = 0; d->pos = 0; d->npos = 0; d->base = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1887; }
    { const char * tb_v1888 = "    if (text) tb_dreindex(d, 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1888; }
    { const char * tb_v1889 = "    else { d->mode = 1; d->nslots = 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1889; }
    { const char * tb_v1890 = "    return d;\n"; *(const char * *)tb_grow(v_lines) = tb_v1890; }
    { const char * tb_v1891 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1891; }
    { const char * tb_v1892 = "// новый ключ int k: можно ли остаться без поиска (mode 1 или 2)\n"; *(const char * *)tb_grow(v_lines) = tb_v1892; }
    { const char * tb_v1893 = "static void tb_dplace_int(tb_dict *d, long long k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1893; }
    { const char * tb_v1894 = "    long long n = d->keys->len;          // уже вместе с новым ключом\n"; *(const char * *)tb_grow(v_lines) = tb_v1894; }
    { const char * tb_v1895 = "    if (d->mode == 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1895; }
    { const char * tb_v1896 = "        if (n == 1) { d->base = k; return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1896; }
    { const char * tb_v1897 = "        if (k == d->base + n - 1) return;  // подряд — остаёмся в mode 1\n"; *(const char * *)tb_grow(v_lines) = tb_v1897; }
    { const char * tb_v1898 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1898; }
    { const char * tb_v1899 = "    if (d->mode == 1 || d->mode == 2) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1899; }
    { const char * tb_v1900 = "        // плотно ли: ключи не дальше 4 ячеек на один ключ\n"; *(const char * *)tb_grow(v_lines) = tb_v1900; }
    { const char * tb_v1901 = "        long long o = k - d->base;\n"; *(const char * *)tb_grow(v_lines) = tb_v1901; }
    { const char * tb_v1902 = "        if (o >= 0 && o < 4 * n + 64) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1902; }
    { const char * tb_v1903 = "            if (d->mode == 2 && o < d->npos) { d->pos[o] = n; return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1903; }
    { const char * tb_v1904 = "            long long want = d->npos * 2 > o + 1 ? d->npos * 2 : o + 1 + n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1904; }
    { const char * tb_v1905 = "            if (want < 16) want = 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v1905; }
    { const char * tb_v1906 = "            tb_dto_pos(d, want);\n"; *(const char * *)tb_grow(v_lines) = tb_v1906; }
    { const char * tb_v1907 = "            return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1907; }
    { const char * tb_v1908 = "        }\n"; *(const char * *)tb_grow(v_lines) = tb_v1908; }
    { const char * tb_v1909 = "        tb_dto_hash(d);\n"; *(const char * *)tb_grow(v_lines) = tb_v1909; }
    { const char * tb_v1910 = "        return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1910; }
    { const char * tb_v1911 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1911; }
    { const char * tb_v1912 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1912; }
    { const char * tb_v1913 = "// место под значение ключа k; если ключа нет — добавить со значением *def\n"; *(const char * *)tb_grow(v_lines) = tb_v1913; }
    { const char * tb_v1914 = "static void *tb_dput(tb_dict *d, const void *k, const void *def) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1914; }
    { const char * tb_v1915 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v1915; }
    { const char * tb_v1916 = "    if (at >= 0) return d->vals->data + at * d->vals->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1916; }
    { const char * tb_v1917 = "    memcpy(tb_grow(d->keys), k, d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1917; }
    { const char * tb_v1918 = "    void *v = tb_grow(d->vals);\n"; *(const char * *)tb_grow(v_lines) = tb_v1918; }
    { const char * tb_v1919 = "    if (def) memcpy(v, def, d->vals->size); else memset(v, 0, d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1919; }
    { const char * tb_v1920 = "    if (d->mode != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1920; }
    { const char * tb_v1921 = "        // без поиска; если ключ лёг далеко, tb_dplace_int сам перейдёт на хеш и впишет все ключи\n"; *(const char * *)tb_grow(v_lines) = tb_v1921; }
    { const char * tb_v1922 = "        tb_dplace_int(d, *(const long long *)k);\n"; *(const char * *)tb_grow(v_lines) = tb_v1922; }
    { const char * tb_v1923 = "        return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v1923; }
    { const char * tb_v1924 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1924; }
    { const char * tb_v1925 = "    if (d->keys->len * 2 > d->nslots) tb_dreindex(d, d->nslots * 2);\n"; *(const char * *)tb_grow(v_lines) = tb_v1925; }
    { const char * tb_v1926 = "    else {\n"; *(const char * *)tb_grow(v_lines) = tb_v1926; }
    { const char * tb_v1927 = "        unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1927; }
    { const char * tb_v1928 = "        while (d->slots[i] != 0) i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1928; }
    { const char * tb_v1929 = "        d->slots[i] = d->keys->len;\n"; *(const char * *)tb_grow(v_lines) = tb_v1929; }
    { const char * tb_v1930 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1930; }
    { const char * tb_v1931 = "    return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v1931; }
    { const char * tb_v1932 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1932; }
    { const char * tb_v1933 = "static void *tb_dget(tb_dict *d, const void *k, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1933; }
    { const char * tb_v1934 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v1934; }
    { const char * tb_v1935 = "    if (TB_RARE(at < 0)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1935; }
    { const char * tb_v1936 = "        if (d->text) tb_crash(line, tb_cat(\"в dict нет ключа: \", *(const char * const *)k));\n"; *(const char * *)tb_grow(v_lines) = tb_v1936; }
    { const char * tb_v1937 = "        tb_crash(line, tb_cat(\"в dict нет ключа: \", tb_itos(*(const long long *)k)));\n"; *(const char * *)tb_grow(v_lines) = tb_v1937; }
    { const char * tb_v1938 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1938; }
    { const char * tb_v1939 = "    return d->vals->data + at * d->vals->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1939; }
    { const char * tb_v1940 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1940; }
    { const char * tb_v1941 = "// together: шаг 1 — просим память принести место ключа заранее\n"; *(const char * *)tb_grow(v_lines) = tb_v1941; }
    { const char * tb_v1942 = "static inline unsigned long long tb_dpre1(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1942; }
    { const char * tb_v1943 = "    if (d->mode == 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1943; }
    { const char * tb_v1944 = "        unsigned long long at = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v1944; }
    { const char * tb_v1945 = "        if (at < (unsigned long long)d->vals->len) __builtin_prefetch(d->vals->data + at * d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1945; }
    { const char * tb_v1946 = "        return ~0ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v1946; }
    { const char * tb_v1947 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1947; }
    { const char * tb_v1948 = "    if (d->mode == 2) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1948; }
    { const char * tb_v1949 = "        unsigned long long o = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v1949; }
    { const char * tb_v1950 = "        if (o < (unsigned long long)d->npos) { __builtin_prefetch(&d->pos[o]); return o; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1950; }
    { const char * tb_v1951 = "        return ~0ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v1951; }
    { const char * tb_v1952 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1952; }
    { const char * tb_v1953 = "    unsigned long long i = tb_hash(d, k) & (d->nslots - 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1953; }
    { const char * tb_v1954 = "    __builtin_prefetch(&d->slots[i]);\n"; *(const char * *)tb_grow(v_lines) = tb_v1954; }
    { const char * tb_v1955 = "    return i;\n"; *(const char * *)tb_grow(v_lines) = tb_v1955; }
    { const char * tb_v1956 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1956; }
    { const char * tb_v1957 = "// шаг 2 — место приехало: просим принести ключ и значение\n"; *(const char * *)tb_grow(v_lines) = tb_v1957; }
    { const char * tb_v1958 = "static inline void tb_dpre2(tb_dict *d, unsigned long long i) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1958; }
    { const char * tb_v1959 = "    if (i == ~0ULL || d->mode == 1) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1959; }
    { const char * tb_v1960 = "    long long s = d->mode == 2 ? d->pos[i] : d->slots[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v1960; }
    { const char * tb_v1961 = "    if (s != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1961; }
    { const char * tb_v1962 = "        if (d->mode == 0) __builtin_prefetch(d->keys->data + (s - 1) * d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1962; }
    { const char * tb_v1963 = "        __builtin_prefetch(d->vals->data + (s - 1) * d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1963; }
    { const char * tb_v1964 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1964; }
    { const char * tb_v1965 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1965; }
    { const char * tb_v1966 = "// элемент списка заранее (prefetch за пределами списка ничего не ломает)\n"; *(const char * *)tb_grow(v_lines) = tb_v1966; }
    { const char * tb_v1967 = "static inline void tb_lpre(tb_list *l, long long i) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1967; }
    { const char * tb_v1968 = "    __builtin_prefetch(l->data + i * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1968; }
    { const char * tb_v1969 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1969; }
    { const char * tb_v1970 = "// ---------- холодные качества obj: своя память, подряд, отдельно от горячих ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1970; }
    { const char * tb_v1971 = "static void *tb_cold_mem(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1971; }
    { const char * tb_v1972 = "    if (tb_temp_depth > 0) return tb_mem(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1972; }
    { const char * tb_v1973 = "    static _Thread_local char *p = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1973; }
    { const char * tb_v1974 = "    static _Thread_local long long left = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1974; }
    { const char * tb_v1975 = "    n = (n + 15) & ~15LL;\n"; *(const char * *)tb_grow(v_lines) = tb_v1975; }
    { const char * tb_v1976 = "    if (left < n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1976; }
    { const char * tb_v1977 = "        long long size = n > 65536 ? n : 65536;\n"; *(const char * *)tb_grow(v_lines) = tb_v1977; }
    { const char * tb_v1978 = "        p = malloc(size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1978; }
    { const char * tb_v1979 = "        if (!p) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1979; }
    { const char * tb_v1980 = "        left = size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1980; }
    { const char * tb_v1981 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1981; }
    { const char * tb_v1982 = "    void *r = p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1982; }
    { const char * tb_v1983 = "    p += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1983; }
    { const char * tb_v1984 = "    left -= n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1984; }
    { const char * tb_v1985 = "    memset(r, 0, n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1985; }
    { const char * tb_v1986 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1986; }
    { const char * tb_v1987 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1987; }
    { const char * tb_v1988 = "// ---------- background: работа в своём потоке ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1988; }
    { const char * tb_v1989 = "typedef struct { pthread_t th; int done; void *res; } tb_task;\n"; *(const char * *)tb_grow(v_lines) = tb_v1989; }
    { const char * tb_v1990 = "static void tb_task_start(tb_task *t, void *(*run)(void *), void *arg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1990; }
    { const char * tb_v1991 = "    t->done = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1991; }
    { const char * tb_v1992 = "    if (pthread_create(&t->th, 0, run, arg) != 0) tb_crash(0, \"не получилось запустить background\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1992; }
    { const char * tb_v1993 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1993; }
    { const char * tb_v1994 = "// a.wait: дождаться потока (один раз) и отдать адрес ответа\n"; *(const char * *)tb_grow(v_lines) = tb_v1994; }
    { const char * tb_v1995 = "static void *tb_wait(tb_task *t) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1995; }
    { const char * tb_v1996 = "    if (!t->done) { pthread_join(t->th, 0); t->done = 1; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1996; }
    { const char * tb_v1997 = "    return t->res;\n"; *(const char * *)tb_grow(v_lines) = tb_v1997; }
    { const char * tb_v1998 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1998; }
    { const char * tb_v1999 = "static int tb_dhas(tb_dict *d, const void *k) { return tb_dfind(d, k) >= 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1999; }
    { const char * tb_v2000 = "static void tb_dremove(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2000; }
    { const char * tb_v2001 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v2001; }
    { const char * tb_v2002 = "    if (at < 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v2002; }
    { const char * tb_v2003 = "    if (d->mode != 0) tb_dto_hash(d);\n"; *(const char * *)tb_grow(v_lines) = tb_v2003; }
    { const char * tb_v2004 = "    long long last = d->keys->len - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2004; }
    { const char * tb_v2005 = "    // на место удалённого ставим последний: порядок меняется только у него\n"; *(const char * *)tb_grow(v_lines) = tb_v2005; }
    { const char * tb_v2006 = "    memcpy(d->keys->data + at * d->keys->size, d->keys->data + last * d->keys->size, d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2006; }
    { const char * tb_v2007 = "    memcpy(d->vals->data + at * d->vals->size, d->vals->data + last * d->vals->size, d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2007; }
    { const char * tb_v2008 = "    d->keys->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2008; }
    { const char * tb_v2009 = "    d->vals->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2009; }
    { const char * tb_v2010 = "    tb_dreindex(d, d->nslots);\n"; *(const char * *)tb_grow(v_lines) = tb_v2010; }
    { const char * tb_v2011 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2011; }
    { const char * tb_v2012 = "static tb_dict *tb_dict_from(int text, long long vsize, long long n, const void *keys, const void *vals) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2012; }
    { const char * tb_v2013 = "    tb_dict *d = tb_dict_new(text, vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v2013; }
    { const char * tb_v2014 = "    long long ksize = d->keys->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v2014; }
    { const char * tb_v2015 = "    for (long long i = 0; i < n; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2015; }
    { const char * tb_v2016 = "        memcpy(tb_dput(d, (const char *)keys + i * ksize, 0), (const char *)vals + i * vsize, vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v2016; }
    { const char * tb_v2017 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2017; }
    { const char * tb_v2018 = "    return d;\n"; *(const char * *)tb_grow(v_lines) = tb_v2018; }
    { const char * tb_v2019 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2019; }
    { const char * tb_v2020 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2020; }
    { const char * tb_v2021 = "// ---------- on all cores: цикл на всех ядрах ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2021; }
    { const char * tb_v2022 = "// Работа делится на куски; каждое ядро берёт следующий свободный кусок,\n"; *(const char * *)tb_grow(v_lines) = tb_v2022; }
    { const char * tb_v2023 = "// пока они не кончатся (так ядра не простаивают, если куски разные по тяжести).\n"; *(const char * *)tb_grow(v_lines) = tb_v2023; }
    { const char * tb_v2024 = "// shared: каждое ядро копит своё на «листке», потом один раз добавляет под замком.\n"; *(const char * *)tb_grow(v_lines) = tb_v2024; }
    { const char * tb_v2025 = "static pthread_mutex_t tb_par_lock = PTHREAD_MUTEX_INITIALIZER;\n"; *(const char * *)tb_grow(v_lines) = tb_v2025; }
    { const char * tb_v2026 = "static int tb_cores(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2026; }
    { const char * tb_v2027 = "    const char *want = getenv(\"TB_CORES\");   // TB_CORES=1 — проверить на одном ядре\n"; *(const char * *)tb_grow(v_lines) = tb_v2027; }
    { const char * tb_v2028 = "    long n = want != 0 ? atol(want) : sysconf(_SC_NPROCESSORS_ONLN);\n"; *(const char * *)tb_grow(v_lines) = tb_v2028; }
    { const char * tb_v2029 = "    return n < 1 ? 1 : (n > 256 ? 256 : (int)n);\n"; *(const char * *)tb_grow(v_lines) = tb_v2029; }
    { const char * tb_v2030 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2030; }
    { const char * tb_v2031 = "static void tb_par_run(void *(*work)(void *), void *job, long long total) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2031; }
    { const char * tb_v2032 = "    int n = tb_cores();\n"; *(const char * *)tb_grow(v_lines) = tb_v2032; }
    { const char * tb_v2033 = "    if (n > total) n = (int)total;\n"; *(const char * *)tb_grow(v_lines) = tb_v2033; }
    { const char * tb_v2034 = "    if (n < 1) n = 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2034; }
    { const char * tb_v2035 = "    pthread_t t[256];\n"; *(const char * *)tb_grow(v_lines) = tb_v2035; }
    { const char * tb_v2036 = "    for (int i = 1; i < n; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2036; }
    { const char * tb_v2037 = "        if (pthread_create(&t[i], 0, work, job) != 0) tb_crash(0, \"не получилось запустить ядро\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2037; }
    { const char * tb_v2038 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2038; }
    { const char * tb_v2039 = "    work(job);\n"; *(const char * *)tb_grow(v_lines) = tb_v2039; }
    { const char * tb_v2040 = "    for (int i = 1; i < n; i++) pthread_join(t[i], 0);\n"; *(const char * *)tb_grow(v_lines) = tb_v2040; }
    { const char * tb_v2041 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2041; }
    { const char * tb_v2042 = "static long long tb_par_step(long long total) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2042; }
    { const char * tb_v2043 = "    long long s = total / ((long long)tb_cores() * 16);\n"; *(const char * *)tb_grow(v_lines) = tb_v2043; }
    { const char * tb_v2044 = "    return s < 1 ? 1 : s;\n"; *(const char * *)tb_grow(v_lines) = tb_v2044; }
    { const char * tb_v2045 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2045; }
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
