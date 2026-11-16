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
static char tb_chars[512];
__attribute__((constructor)) static void tb_chars_init(void) {
    for (int i = 0; i < 256; i++) { tb_chars[2 * i] = (char)i; tb_chars[2 * i + 1] = 0; }
}
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
        (tb_show_4(tb_cat("не могу открыть файл: ", v_path)), tb_ps("\n"));
        tb_exit(1LL);
    }
    const char * v_source = tb_v6;
    { const char * tb_v7 = v_path; const char * *tb_p7 = &(((tb_o_Comp *)tb_nn(v_c, 43))->q_file_name); *tb_p7 = tb_v7; }
    f_lexer__lex(v_c, v_source);
    tb_o_Node * v_part = f_parser__parse(v_c);
    {
        tb_list * tb_l8 = ((tb_o_Node *)tb_nn(v_part, 46))->q_kids;
        for (long long tb_i8 = 0; tb_i8 < tb_l8->len; tb_i8++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l8->data)[tb_i8];
            { const char * tb_v9 = v_name; const char * *tb_p9 = &(((tb_o_Node *)tb_nn(v_k, 47))->q_mod); *tb_p9 = tb_v9; }
            { const char * tb_v10 = v_path; const char * *tb_p10 = &(((tb_o_Node *)tb_nn(v_k, 48))->q_file); *tb_p10 = tb_v10; }
            f_parser__add_kid(v_program, v_k);
        }
    }
    {
        tb_list * tb_l11 = ((tb_o_Node *)tb_nn(v_part, 51))->q_kids;
        for (long long tb_i11 = 0; tb_i11 < tb_l11->len; tb_i11++) {
            tb_o_Node * const v_u = ((tb_o_Node * *)tb_l11->data)[tb_i11];
            if (({ const char *tb_s12 = ((tb_o_Node *)tb_nn(v_u, 52))->q_kind; ((unsigned char)tb_s12[0] == 117 && (unsigned char)tb_s12[1] == 115 && (unsigned char)tb_s12[2] == 101 && tb_s12[3] == 0); })) {
                const char * v_next_path = tb_cat(tb_cat(f_main__folder_of(v_path), ((tb_o_Node *)tb_nn(v_u, 53))->q_name), ".tb");
                f_main__load_module(v_c, v_program, v_next_path, ((tb_o_Node *)tb_nn(v_u, 54))->q_line, v_path, v_loaded);
            }
        }
    }
}

static tb_o_Node * f_main__load_program(tb_o_Comp * v_c, const char * v_path) {
    tb_o_Node * v_program = f_parser__new_node("program", 1LL);
    tb_list * v_loaded = tb_list_new(sizeof(const char *));
    f_main__load_module(v_c, v_program, v_path, 0LL, v_path, v_loaded);
    { const char * tb_v13 = v_path; const char * *tb_p13 = &(((tb_o_Comp *)tb_nn(v_c, 60))->q_file_name); *tb_p13 = tb_v13; }
    return v_program;
    tb_crash(56, "функция load_program дошла до конца без return");
    return 0;
}

static const char * f_main__quote(const char * v_s) {
    return tb_cat(tb_cat("'", tb_replace(v_s, "'", "'\\''", 65)), "'");
    tb_crash(64, "функция quote дошла до конца без return");
    return "";
}

static _Bool f_main__is_folder(const char * v_path) {
    return (tb_run(tb_cat("test -d ", f_main__quote(v_path))) == 0LL);
    tb_crash(67, "функция is_folder дошла до конца без return");
    return 0;
}

static _Bool f_main__is_file(const char * v_path) {
    return (tb_run(tb_cat("test -f ", f_main__quote(v_path))) == 0LL);
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
    if (({ const char *tb_s14 = v_name; (tb_s14[0] == 0); })) {
        v_name = ".";
    }
    while (((tb_tlen(v_name) > 1LL) && tb_ends(v_name, "/"))) {
        v_name = tb_cut(v_name, 0LL, (tb_tlen(v_name) - 2LL), 86);
    }
    if (f_main__is_folder(v_name)) {
        v_main_tb = tb_cat(v_name, "/main.tb");
        if ((!f_main__is_file(v_main_tb))) {
            f_main__say_error(tb_cat(tb_cat("в папке ", v_name), " нет main.tb — с него начинается проект"));
        }
        const char * v_base = tb_cut(v_name, tb_tlen(f_main__folder_of(v_name)), (tb_tlen(v_name) - 1LL), 91);
        const char * v_build_dir = tb_cat(v_name, "/build");
        long long v_code = tb_run(tb_cat("mkdir -p ", f_main__quote(v_build_dir)));
        if (({ const char *tb_s15 = v_name; ((unsigned char)tb_s15[0] == 46 && tb_s15[1] == 0); })) {
            v_code = tb_run(tb_cat("basename \"$PWD\" > ", f_main__quote(tb_cat(v_build_dir, "/.name"))));
            v_base = tb_trim((tb_t1 = tb_read_file(tb_cat(v_build_dir, "/.name")), tb_err ? (tb_err = 0, "") : tb_t1));
        }
        if (((({ const char *tb_s16 = v_base; (tb_s16[0] == 0); }) || ({ const char *tb_s17 = v_base; ((unsigned char)tb_s17[0] == 46 && tb_s17[1] == 0); })) || ({ const char *tb_s18 = v_base; ((unsigned char)tb_s18[0] == 46 && (unsigned char)tb_s18[1] == 46 && tb_s18[2] == 0); }))) {
            v_base = "main";
        }
        v_exe = tb_cat(tb_cat(v_build_dir, "/"), v_base);
        return (tb_r_f_main__project_main){v_main_tb, v_exe};
    }
    if (f_main__is_file(v_name)) {
        v_main_tb = v_name;
        v_exe = f_main__without_tb(v_name);
        return (tb_r_f_main__project_main){v_main_tb, v_exe};
    }
    if (f_main__is_file(tb_cat(v_name, ".tb"))) {
        v_main_tb = tb_cat(v_name, ".tb");
        v_exe = v_name;
        return (tb_r_f_main__project_main){v_main_tb, v_exe};
    }
    (tb_show_4(tb_cat("нет проекта или файла: ", v_name)), tb_ps("\n"));
    f_main__say_error(tb_cat("новый проект делается так:  tb new ", v_name));
    return (tb_r_f_main__project_main){v_main_tb, v_exe};
}

static void f_main__new_project(const char * v_name) {
    if (({ const char *tb_s19 = v_name; (tb_s19[0] == 0); })) {
        f_main__say_error("напиши имя проекта:  tb new имя");
    }
    if ((tb_run(tb_cat("test -e ", f_main__quote(v_name))) == 0LL)) {
        f_main__say_error(tb_cat(tb_cat("уже есть папка или файл с именем ", v_name), " — возьми другое имя"));
    }
    if ((tb_run(tb_cat("mkdir ", f_main__quote(v_name))) != 0LL)) {
        f_main__say_error(tb_cat("не могу сделать папку: ", v_name));
    }
    tb_write_file(tb_cat(v_name, "/main.tb"), tb_cat(tb_cat("func main()\n    print(\"Привет из проекта ", v_name), "!\")\n"));
    if (tb_err != 0) {
        const char * const v_err = tb_err;
        tb_err = 0;
        f_main__say_error(tb_cat(tb_cat("не могу записать файл: ", v_name), "/main.tb"));
    }
    tb_write_file(tb_cat(v_name, "/.gitignore"), "build/\n");
    if (tb_err != 0) {
        const char * const v_err = tb_err;
        tb_err = 0;
        const char * v_ignored = v_err;
    }
    (tb_show_4(tb_cat(tb_cat("готов проект ", v_name), ":")), tb_ps("\n"));
    (tb_show_4(tb_cat(tb_cat("  ", v_name), "/main.tb   — здесь пишешь программу")), tb_ps("\n"));
    (tb_show_4(tb_cat(tb_cat("  ", v_name), "/build/    — сюда кладётся готовая программа\n")), tb_ps("\n"));
    (tb_show_4(tb_cat("запуск:  tb start ", v_name)), tb_ps("\n"));
}

static long long f_main__build(tb_o_Comp * v_c, const char * v_path, const char * v_exe) {
    const char * v_c_path = tb_cat(v_exe, ".c");
    tb_o_Node * v_program = f_main__load_program(v_c, v_path);
    f_check__check(v_c, v_program);
    f_emit__emit_c(v_c, v_program);
    tb_write_file(v_c_path, tb_join(((tb_o_Comp *)tb_nn(v_c, 133))->q_out, ""));
    if (tb_err != 0) {
        const char * const v_err = tb_err;
        tb_err = 0;
        (tb_show_4(tb_cat("не могу записать файл: ", v_c_path)), tb_ps("\n"));
        return 1LL;
    }
    const char * v_command = tb_cat(tb_cat(tb_cat(tb_cat("${CC:-cc} -O2 -w -o ", f_main__quote(v_exe)), " "), f_main__quote(v_c_path)), " -lm -lpthread");
    if ((tb_run(v_command) != 0LL)) {
        (tb_show_4(tb_cat("компилятор C не справился (это ошибка Tachbit, а не твоя): ", v_c_path)), tb_ps("\n"));
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
    if (({ const char *tb_s23 = v_mode; ((unsigned char)tb_s23[0] == 110 && (unsigned char)tb_s23[1] == 101 && (unsigned char)tb_s23[2] == 119 && tb_s23[3] == 0); })) {
        const char * v_name = "";
        if (((v_a)->len > 1LL)) {
            v_name = (*(const char * *)tb_at(v_a, 1LL, sizeof(const char *), 162));
        }
        f_main__new_project(v_name);
        return;
    }
    if (((({ const char *tb_s24 = v_mode; ((unsigned char)tb_s24[0] == 104 && (unsigned char)tb_s24[1] == 101 && (unsigned char)tb_s24[2] == 108 && (unsigned char)tb_s24[3] == 112 && tb_s24[4] == 0); }) || ({ const char *tb_s25 = v_mode; ((unsigned char)tb_s25[0] == 45 && (unsigned char)tb_s25[1] == 104 && tb_s25[2] == 0); })) || ({ const char *tb_s26 = v_mode; ((unsigned char)tb_s26[0] == 45 && (unsigned char)tb_s26[1] == 45 && (unsigned char)tb_s26[2] == 104 && (unsigned char)tb_s26[3] == 101 && (unsigned char)tb_s26[4] == 108 && (unsigned char)tb_s26[5] == 112 && tb_s26[6] == 0); }))) {
        f_main__usage();
        return;
    }
    if ((({ const char *tb_s27 = v_mode; ((unsigned char)tb_s27[0] == 118 && (unsigned char)tb_s27[1] == 101 && (unsigned char)tb_s27[2] == 114 && (unsigned char)tb_s27[3] == 115 && (unsigned char)tb_s27[4] == 105 && (unsigned char)tb_s27[5] == 111 && (unsigned char)tb_s27[6] == 110 && tb_s27[7] == 0); }) || (strcmp(v_mode, "--version") == 0))) {
        (tb_show_4("Tachbit 0.7 (компилятор на Tachbit)"), tb_ps("\n"));
        return;
    }
    if (({ const char *tb_s28 = v_mode; ((unsigned char)tb_s28[0] == 115 && (unsigned char)tb_s28[1] == 116 && (unsigned char)tb_s28[2] == 97 && (unsigned char)tb_s28[3] == 114 && (unsigned char)tb_s28[4] == 116 && tb_s28[5] == 0); })) {
        v_mode = "run";
    }
    if ((((v_a)->len < 2LL) && (!(((({ const char *tb_s29 = v_mode; ((unsigned char)tb_s29[0] == 114 && (unsigned char)tb_s29[1] == 117 && (unsigned char)tb_s29[2] == 110 && tb_s29[3] == 0); }) || ({ const char *tb_s30 = v_mode; ((unsigned char)tb_s30[0] == 98 && (unsigned char)tb_s30[1] == 117 && (unsigned char)tb_s30[2] == 105 && (unsigned char)tb_s30[3] == 108 && (unsigned char)tb_s30[4] == 100 && tb_s30[5] == 0); })) || ({ const char *tb_s31 = v_mode; ((unsigned char)tb_s31[0] == 99 && (unsigned char)tb_s31[1] == 104 && (unsigned char)tb_s31[2] == 101 && (unsigned char)tb_s31[3] == 99 && (unsigned char)tb_s31[4] == 107 && tb_s31[5] == 0); })) || ({ const char *tb_s32 = v_mode; ((unsigned char)tb_s32[0] == 99 && tb_s32[1] == 0); }))))) {
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
        tb_r_f_main__project_main tb_m33 = f_main__project_main(v_arg);
        v_path = tb_m33.o0;
        v_exe = tb_m33.o1;
    }
    tb_o_Comp * v_c = tb_new_Comp(v_path, tb_list_new(sizeof(tb_o_Token *)), 0LL, tb_list_new(sizeof(tb_o_Type *)), tb_list_new(sizeof(tb_o_Func *)), tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Use *)), tb_list_new(sizeof(const char *)), "", tb_list_new(sizeof(tb_o_Var *)), 0LL, 0LL, 0LL, 0LL, tb_list_new(sizeof(long long)), 0LL, 0, 0LL, 0LL, 0, 0, tb_list_new(sizeof(const char *)), 0LL, 0, tb_list_new(sizeof(const char *)), tb_list_new(sizeof(const char *)), tb_list_new(sizeof(const char *)), 0LL, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(const char *)), tb_list_new(sizeof(long long)), 0LL, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(long long)), tb_bits_new(), tb_list_new(sizeof(long long)), tb_list_new(sizeof(long long)), tb_bits_new(), tb_list_new(sizeof(const char *)));
    if (({ const char *tb_s34 = v_mode; ((unsigned char)tb_s34[0] == 116 && (unsigned char)tb_s34[1] == 111 && (unsigned char)tb_s34[2] == 107 && (unsigned char)tb_s34[3] == 101 && (unsigned char)tb_s34[4] == 110 && (unsigned char)tb_s34[5] == 115 && tb_s34[6] == 0); })) {
        const char * tb_v35 = tb_read_file(v_path);
        if (tb_err != 0) {
            const char * const v_err = tb_err;
            tb_err = 0;
            f_main__say_error(tb_cat("не могу открыть файл: ", v_path));
        }
        const char * v_src = tb_v35;
        f_lexer__lex(v_c, v_src);
        f_lexer__print_tokens(v_c);
        return;
    }
    if (({ const char *tb_s36 = v_mode; ((unsigned char)tb_s36[0] == 116 && (unsigned char)tb_s36[1] == 114 && (unsigned char)tb_s36[2] == 101 && (unsigned char)tb_s36[3] == 101 && tb_s36[4] == 0); })) {
        tb_list * v_out = tb_list_new(sizeof(const char *));
        f_parser__tree_lines(f_main__load_program(v_c, v_path), 0LL, v_out);
        (tb_show_4(tb_join(v_out, "\n")), tb_ps("\n"));
        return;
    }
    if (({ const char *tb_s37 = v_mode; ((unsigned char)tb_s37[0] == 99 && (unsigned char)tb_s37[1] == 104 && (unsigned char)tb_s37[2] == 101 && (unsigned char)tb_s37[3] == 99 && (unsigned char)tb_s37[4] == 107 && tb_s37[5] == 0); })) {
        f_check__check(v_c, f_main__load_program(v_c, v_path));
        (tb_show_4("ошибок нет"), tb_ps("\n"));
        return;
    }
    if (({ const char *tb_s38 = v_mode; ((unsigned char)tb_s38[0] == 99 && tb_s38[1] == 0); })) {
        tb_o_Node * v_program = f_main__load_program(v_c, v_path);
        f_check__check(v_c, v_program);
        f_emit__emit_c(v_c, v_program);
        (tb_show_4(tb_cut(tb_join(((tb_o_Comp *)tb_nn(v_c, 201))->q_out, ""), 0LL, (tb_tlen(tb_join(((tb_o_Comp *)tb_nn(v_c, 201))->q_out, "")) - 2LL), 201)), tb_ps("\n"));
        return;
    }
    if (({ const char *tb_s39 = v_mode; ((unsigned char)tb_s39[0] == 98 && (unsigned char)tb_s39[1] == 117 && (unsigned char)tb_s39[2] == 105 && (unsigned char)tb_s39[3] == 108 && (unsigned char)tb_s39[4] == 100 && tb_s39[5] == 0); })) {
        tb_exit(f_main__build(v_c, v_path, v_exe));
    }
    if (({ const char *tb_s40 = v_mode; ((unsigned char)tb_s40[0] == 114 && (unsigned char)tb_s40[1] == 117 && (unsigned char)tb_s40[2] == 110 && tb_s40[3] == 0); })) {
        if ((f_main__build(v_c, v_path, v_exe) != 0LL)) {
            tb_exit(1LL);
        }
        const char * v_command = f_main__quote(v_exe); tb_rl_command = -1;
        if ((tb_find(v_exe, "/") < 0LL)) {
            v_command = f_main__quote(tb_cat("./", v_exe)); tb_rl_command = -1;
        }
        {
            long long tb_end41 = ((v_a)->len - 1LL);
            for (long long v_i = 2LL; v_i <= tb_end41; v_i++) {
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
        v_place = tb_cat(tb_cat(((tb_o_Comp *)tb_nn(v_c, 9))->q_file_name, ", строка "), tb_itos(v_line));
    }
    const char * v_s = tb_cat(tb_cat(v_place, ": "), v_message); tb_rl_s = -1;
    if (({ const char *tb_s42 = v_detail; !(tb_s42[0] == 0); })) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, ": ", sizeof(": ") - 1); tb_app(&v_s, &tb_rl_s, &tb_rc_s, v_detail); 
    }
    if (v_color) {
        const char * v_esc = tb_chr(27LL);
        v_s = tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_esc, "[1m"), v_place), ":"), v_esc), "[0m "), v_esc), "[31m"), v_message), v_esc), "[0m"); tb_rl_s = -1;
        if (({ const char *tb_s43 = v_detail; !(tb_s43[0] == 0); })) {
            tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, ": ", sizeof(": ") - 1); tb_app(&v_s, &tb_rl_s, &tb_rc_s, v_esc); tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, "[33m", sizeof("[33m") - 1); tb_app(&v_s, &tb_rl_s, &tb_rc_s, v_detail); tb_app(&v_s, &tb_rl_s, &tb_rc_s, v_esc); tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, "[0m", sizeof("[0m") - 1); 
        }
    }
    (tb_show_4(v_s), tb_ps("\n"));
    tb_exit(1LL);
}

static _Bool f_lexer__is_command(const char * v_word) {
    tb_list * v_commands = tb_list_from(40, sizeof(const char *), (const char *[]){"func", "return", "gives", "if", "else", "each", "in", "count", "from", "to", "repeat", "times", "while", "skip", "stop", "const", "shared", "obj", "link", "int", "dec", "text", "bool", "and", "or", "not", "true", "false", "use", "modpub", "modpri", "temp", "together", "background", "danger", "by", "list", "dict", "fail", "try"});
    {
        tb_list * tb_l44 = v_commands;
        for (long long tb_i44 = 0; tb_i44 < tb_l44->len; tb_i44++) {
            const char * const v_c = ((const char * *)tb_l44->data)[tb_i44];
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
    { tb_o_Token * tb_v45 = tb_new_Token(v_kind, v_word, v_line); *(tb_o_Token * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 26))->q_tokens) = tb_v45; }
}

static _Bool f_lexer__two_char_sign(const char * v_a, const char * v_b) {
    if ((({ const char *tb_s46 = v_b; ((unsigned char)tb_s46[0] == 61 && tb_s46[1] == 0); }) && (((((((({ const char *tb_s47 = v_a; ((unsigned char)tb_s47[0] == 43 && tb_s47[1] == 0); }) || ({ const char *tb_s48 = v_a; ((unsigned char)tb_s48[0] == 45 && tb_s48[1] == 0); })) || ({ const char *tb_s49 = v_a; ((unsigned char)tb_s49[0] == 42 && tb_s49[1] == 0); })) || ({ const char *tb_s50 = v_a; ((unsigned char)tb_s50[0] == 47 && tb_s50[1] == 0); })) || ({ const char *tb_s51 = v_a; ((unsigned char)tb_s51[0] == 61 && tb_s51[1] == 0); })) || ({ const char *tb_s52 = v_a; ((unsigned char)tb_s52[0] == 33 && tb_s52[1] == 0); })) || ({ const char *tb_s53 = v_a; ((unsigned char)tb_s53[0] == 60 && tb_s53[1] == 0); })) || ({ const char *tb_s54 = v_a; ((unsigned char)tb_s54[0] == 62 && tb_s54[1] == 0); })))) {
        return 1;
    }
    if ((({ const char *tb_s55 = v_a; ((unsigned char)tb_s55[0] == 45 && tb_s55[1] == 0); }) && ({ const char *tb_s56 = v_b; ((unsigned char)tb_s56[0] == 62 && tb_s56[1] == 0); }))) {
        return 1;
    }
    if ((({ const char *tb_s57 = v_a; ((unsigned char)tb_s57[0] == 46 && tb_s57[1] == 0); }) && ({ const char *tb_s58 = v_b; ((unsigned char)tb_s58[0] == 46 && tb_s58[1] == 0); }))) {
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
    { tb_list * tb_v59 = v_fresh; tb_list * *tb_p59 = &(((tb_o_Comp *)tb_nn(v_c, 43))->q_tokens); *tb_p59 = tb_v59; }
    tb_list * v_indents = tb_list_from(1, sizeof(long long), (long long[]){0LL});
    tb_list * v_lines = tb_split(v_src, "\n", 45);
    long long v_line = 0LL;
    {
        tb_list * tb_l60 = v_lines;
        for (long long tb_i60 = 0; tb_i60 < tb_l60->len; tb_i60++) {
            const char * const v_s = ((const char * *)tb_l60->data)[tb_i60];
            { long long tb_v61 = 1LL; long long *tb_p61 = &(v_line); *tb_p61 += tb_v61; }
            long long v_n = tb_tlen(v_s);
            long long v_spaces = 0LL;
            long long v_pos = 0LL;
            while (((v_pos < v_n) && ((f_lexer__at(v_s, v_pos) == 32LL) || (f_lexer__at(v_s, v_pos) == 9LL)))) {
                if ((f_lexer__at(v_s, v_pos) == 9LL)) {
                    { long long tb_v62 = 4LL; long long *tb_p62 = &(v_spaces); *tb_p62 += tb_v62; }
                } else {
                    { long long tb_v63 = 1LL; long long *tb_p63 = &(v_spaces); *tb_p63 += tb_v63; }
                }
                { long long tb_v64 = 1LL; long long *tb_p64 = &(v_pos); *tb_p64 += tb_v64; }
            }
            if ((((v_pos >= v_n) || (f_lexer__at(v_s, v_pos) == 13LL)) || ((f_lexer__at(v_s, v_pos) == 47LL) && (f_lexer__at(v_s, (v_pos + 1LL)) == 47LL)))) {
                continue;
            }
            long long v_top = (*(long long *)tb_at(v_indents, ((v_indents)->len - 1LL), sizeof(long long), 62));
            if ((v_spaces > v_top)) {
                if (((v_indents)->len == 200LL)) {
                    f_errors__error_at(v_c, v_line, "слишком глубокая вложенность", "");
                }
                { long long tb_v65 = v_spaces; *(long long *)tb_grow(v_indents) = tb_v65; }
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
                    { long long tb_v66 = 1LL; long long *tb_p66 = &(v_pos); *tb_p66 += tb_v66; }
                    continue;
                }
                if (((v_ch == 47LL) && (f_lexer__at(v_s, (v_pos + 1LL)) == 47LL))) {
                    break;
                }
                long long v_start = v_pos;
                if (f_lexer__is_letter(v_ch)) {
                    while ((f_lexer__is_letter(f_lexer__at(v_s, v_pos)) || f_lexer__is_digit(f_lexer__at(v_s, v_pos)))) {
                        { long long tb_v67 = 1LL; long long *tb_p67 = &(v_pos); *tb_p67 += tb_v67; }
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
                        { long long tb_v68 = 1LL; long long *tb_p68 = &(v_pos); *tb_p68 += tb_v68; }
                    }
                    if (((f_lexer__at(v_s, v_pos) == 46LL) && f_lexer__is_digit(f_lexer__at(v_s, (v_pos + 1LL))))) {
                        v_kind = "decimal";
                        { long long tb_v69 = 1LL; long long *tb_p69 = &(v_pos); *tb_p69 += tb_v69; }
                        while (f_lexer__is_digit(f_lexer__at(v_s, v_pos))) {
                            { long long tb_v70 = 1LL; long long *tb_p70 = &(v_pos); *tb_p70 += tb_v70; }
                        }
                    }
                    f_lexer__add_token(v_c, v_kind, tb_cut(v_s, v_start, (v_pos - 1LL), 101), v_line);
                } else if ((v_ch == 34LL)) {
                    { long long tb_v71 = 1LL; long long *tb_p71 = &(v_pos); *tb_p71 += tb_v71; }
                    while (((v_pos < v_n) && (f_lexer__at(v_s, v_pos) != 34LL))) {
                        if ((f_lexer__at(v_s, v_pos) == 92LL)) {
                            { long long tb_v72 = 1LL; long long *tb_p72 = &(v_pos); *tb_p72 += tb_v72; }
                        }
                        { long long tb_v73 = 1LL; long long *tb_p73 = &(v_pos); *tb_p73 += tb_v73; }
                    }
                    if ((f_lexer__at(v_s, v_pos) != 34LL)) {
                        f_errors__error_at(v_c, v_line, "текст в кавычках не закрыт", "");
                    }
                    { long long tb_v74 = 1LL; long long *tb_p74 = &(v_pos); *tb_p74 += tb_v74; }
                    f_lexer__add_token(v_c, "string", tb_cut(v_s, v_start, (v_pos - 1LL), 111), v_line);
                } else {
                    const char * v_a = tb_char_at(v_s, v_pos, -1, 113);
                    const char * v_b = "";
                    if (((v_pos + 1LL) < v_n)) {
                        v_b = tb_char_at(v_s, (v_pos + 1LL), -1, 116);
                    }
                    if (f_lexer__two_char_sign(v_a, v_b)) {
                        f_lexer__add_token(v_c, "sign", tb_cat(v_a, v_b), v_line);
                        { long long tb_v75 = 2LL; long long *tb_p75 = &(v_pos); *tb_p75 += tb_v75; }
                    } else if (f_lexer__one_char_sign(v_a)) {
                        f_lexer__add_token(v_c, "sign", v_a, v_line);
                        { long long tb_v76 = 1LL; long long *tb_p76 = &(v_pos); *tb_p76 += tb_v76; }
                    } else {
                        f_errors__error_at(v_c, v_line, "непонятный символ", v_a);
                    }
                }
            }
            f_lexer__add_token(v_c, "newline", "", v_line);
        }
    }
    if (((tb_tlen(v_src) > 0LL) && tb_ends(v_src, "\n"))) {
        { long long tb_v77 = 1LL; long long *tb_p77 = &(v_line); *tb_p77 -= tb_v77; }
    }
    while (((v_indents)->len > 1LL)) {
        long long v_gone = (*(long long *)tb_pop(v_indents, 130));
        f_lexer__add_token(v_c, "dedent", "", (v_line + 1LL));
    }
    f_lexer__add_token(v_c, "end", "", (v_line + 1LL));
}

static const char * f_lexer__token_kind_name(const char * v_kind) {
    if (({ const char *tb_s78 = v_kind; ((unsigned char)tb_s78[0] == 110 && (unsigned char)tb_s78[1] == 97 && (unsigned char)tb_s78[2] == 109 && (unsigned char)tb_s78[3] == 101 && tb_s78[4] == 0); })) {
        return "ИМЯ";
    }
    if (({ const char *tb_s79 = v_kind; ((unsigned char)tb_s79[0] == 99 && (unsigned char)tb_s79[1] == 111 && (unsigned char)tb_s79[2] == 109 && (unsigned char)tb_s79[3] == 109 && (unsigned char)tb_s79[4] == 97 && (unsigned char)tb_s79[5] == 110 && (unsigned char)tb_s79[6] == 100 && tb_s79[7] == 0); })) {
        return "КОМАНДА";
    }
    if (({ const char *tb_s80 = v_kind; ((unsigned char)tb_s80[0] == 110 && (unsigned char)tb_s80[1] == 117 && (unsigned char)tb_s80[2] == 109 && (unsigned char)tb_s80[3] == 98 && (unsigned char)tb_s80[4] == 101 && (unsigned char)tb_s80[5] == 114 && tb_s80[6] == 0); })) {
        return "ЧИСЛО";
    }
    if (({ const char *tb_s81 = v_kind; ((unsigned char)tb_s81[0] == 100 && (unsigned char)tb_s81[1] == 101 && (unsigned char)tb_s81[2] == 99 && (unsigned char)tb_s81[3] == 105 && (unsigned char)tb_s81[4] == 109 && (unsigned char)tb_s81[5] == 97 && (unsigned char)tb_s81[6] == 108 && tb_s81[7] == 0); })) {
        return "ДРОБЬ";
    }
    if (({ const char *tb_s82 = v_kind; ((unsigned char)tb_s82[0] == 115 && (unsigned char)tb_s82[1] == 116 && (unsigned char)tb_s82[2] == 114 && (unsigned char)tb_s82[3] == 105 && (unsigned char)tb_s82[4] == 110 && (unsigned char)tb_s82[5] == 103 && tb_s82[6] == 0); })) {
        return "ТЕКСТ";
    }
    if (({ const char *tb_s83 = v_kind; ((unsigned char)tb_s83[0] == 115 && (unsigned char)tb_s83[1] == 105 && (unsigned char)tb_s83[2] == 103 && (unsigned char)tb_s83[3] == 110 && tb_s83[4] == 0); })) {
        return "ЗНАК";
    }
    if (({ const char *tb_s84 = v_kind; ((unsigned char)tb_s84[0] == 110 && (unsigned char)tb_s84[1] == 101 && (unsigned char)tb_s84[2] == 119 && (unsigned char)tb_s84[3] == 108 && (unsigned char)tb_s84[4] == 105 && (unsigned char)tb_s84[5] == 110 && (unsigned char)tb_s84[6] == 101 && tb_s84[7] == 0); })) {
        return "КОНЕЦ СТРОКИ";
    }
    if (({ const char *tb_s85 = v_kind; ((unsigned char)tb_s85[0] == 105 && (unsigned char)tb_s85[1] == 110 && (unsigned char)tb_s85[2] == 100 && (unsigned char)tb_s85[3] == 101 && (unsigned char)tb_s85[4] == 110 && (unsigned char)tb_s85[5] == 116 && tb_s85[6] == 0); })) {
        return "ОТСТУП";
    }
    if (({ const char *tb_s86 = v_kind; ((unsigned char)tb_s86[0] == 100 && (unsigned char)tb_s86[1] == 101 && (unsigned char)tb_s86[2] == 100 && (unsigned char)tb_s86[3] == 101 && (unsigned char)tb_s86[4] == 110 && (unsigned char)tb_s86[5] == 116 && tb_s86[6] == 0); })) {
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
        tb_list * tb_l87 = ((tb_o_Comp *)tb_nn(v_c, 158))->q_tokens;
        for (long long tb_i87 = 0; tb_i87 < tb_l87->len; tb_i87++) {
            tb_o_Token * const v_t = ((tb_o_Token * *)tb_l87->data)[tb_i87];
            const char * v_num = tb_cat("", tb_itos(((tb_o_Token *)tb_nn(v_t, 159))->q_line));
            while ((tb_tlen(v_num) < 3LL)) {
                v_num = tb_cat(" ", v_num);
            }
            const char * v_name = f_lexer__token_kind_name(((tb_o_Token *)tb_nn(v_t, 162))->q_kind); tb_rl_name = -1;
            while (((tb_rl_name >= 0 ? tb_rl_name : tb_tlen(v_name)) < 13LL)) {
                tb_app_n(&v_name, &tb_rl_name, &tb_rc_name, " ", sizeof(" ") - 1); 
            }
            { const char * tb_v88 = tb_cat(tb_cat(tb_cat(tb_cat(v_num, ": "), v_name), " "), ((tb_o_Token *)tb_nn(v_t, 165))->q_word); *(const char * *)tb_grow(v_out) = tb_v88; }
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
    { tb_o_Node * tb_v89 = v_kid; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_parent, 11))->q_kids) = tb_v89; }
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
    if (({ const char *tb_s90 = ((tb_o_Token *)tb_nn((*(tb_o_Token * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 27))->q_tokens, ((tb_o_Comp *)tb_nn(v_c, 27))->q_pos, sizeof(tb_o_Token *), 27)), 27))->q_kind; !((unsigned char)tb_s90[0] == 101 && (unsigned char)tb_s90[1] == 110 && (unsigned char)tb_s90[2] == 100 && tb_s90[3] == 0); })) {
        { long long tb_v91 = 1LL; long long *tb_p91 = &(((tb_o_Comp *)tb_nn(v_c, 28))->q_pos); *tb_p91 += tb_v91; }
    }
}

static _Bool f_parser__is_sign(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__now(v_c);
    return (({ const char *tb_s92 = ((tb_o_Token *)tb_nn(v_t, 32))->q_kind; ((unsigned char)tb_s92[0] == 115 && (unsigned char)tb_s92[1] == 105 && (unsigned char)tb_s92[2] == 103 && (unsigned char)tb_s92[3] == 110 && tb_s92[4] == 0); }) && (strcmp(((tb_o_Token *)tb_nn(v_t, 32))->q_word, v_s) == 0));
    tb_crash(30, "функция is_sign дошла до конца без return");
    return 0;
}

static _Bool f_parser__is_cmd(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__now(v_c);
    return (({ const char *tb_s93 = ((tb_o_Token *)tb_nn(v_t, 36))->q_kind; ((unsigned char)tb_s93[0] == 99 && (unsigned char)tb_s93[1] == 111 && (unsigned char)tb_s93[2] == 109 && (unsigned char)tb_s93[3] == 109 && (unsigned char)tb_s93[4] == 97 && (unsigned char)tb_s93[5] == 110 && (unsigned char)tb_s93[6] == 100 && tb_s93[7] == 0); }) && (strcmp(((tb_o_Token *)tb_nn(v_t, 36))->q_word, v_s) == 0));
    tb_crash(34, "функция is_cmd дошла до конца без return");
    return 0;
}

static _Bool f_parser__ahead_is_sign(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__ahead(v_c);
    return (({ const char *tb_s94 = ((tb_o_Token *)tb_nn(v_t, 40))->q_kind; ((unsigned char)tb_s94[0] == 115 && (unsigned char)tb_s94[1] == 105 && (unsigned char)tb_s94[2] == 103 && (unsigned char)tb_s94[3] == 110 && tb_s94[4] == 0); }) && (strcmp(((tb_o_Token *)tb_nn(v_t, 40))->q_word, v_s) == 0));
    tb_crash(38, "функция ahead_is_sign дошла до конца без return");
    return 0;
}

static const char * f_parser__what_is_here(tb_o_Comp * v_c) {
    const char * v_k = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 43))->q_kind;
    if (({ const char *tb_s95 = v_k; ((unsigned char)tb_s95[0] == 110 && (unsigned char)tb_s95[1] == 101 && (unsigned char)tb_s95[2] == 119 && (unsigned char)tb_s95[3] == 108 && (unsigned char)tb_s95[4] == 105 && (unsigned char)tb_s95[5] == 110 && (unsigned char)tb_s95[6] == 101 && tb_s95[7] == 0); })) {
        return "конец строки";
    }
    if (({ const char *tb_s96 = v_k; ((unsigned char)tb_s96[0] == 105 && (unsigned char)tb_s96[1] == 110 && (unsigned char)tb_s96[2] == 100 && (unsigned char)tb_s96[3] == 101 && (unsigned char)tb_s96[4] == 110 && (unsigned char)tb_s96[5] == 116 && tb_s96[6] == 0); })) {
        return "лишний отступ";
    }
    if (({ const char *tb_s97 = v_k; ((unsigned char)tb_s97[0] == 100 && (unsigned char)tb_s97[1] == 101 && (unsigned char)tb_s97[2] == 100 && (unsigned char)tb_s97[3] == 101 && (unsigned char)tb_s97[4] == 110 && (unsigned char)tb_s97[5] == 116 && tb_s97[6] == 0); })) {
        return "конец блока";
    }
    if (({ const char *tb_s98 = v_k; ((unsigned char)tb_s98[0] == 101 && (unsigned char)tb_s98[1] == 110 && (unsigned char)tb_s98[2] == 100 && tb_s98[3] == 0); })) {
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
    if (({ const char *tb_s99 = ((tb_o_Token *)tb_nn(v_t, 66))->q_kind; !((unsigned char)tb_s99[0] == 110 && (unsigned char)tb_s99[1] == 97 && (unsigned char)tb_s99[2] == 109 && (unsigned char)tb_s99[3] == 101 && tb_s99[4] == 0); })) {
        if (({ const char *tb_s100 = ((tb_o_Token *)tb_nn(v_t, 67))->q_kind; ((unsigned char)tb_s100[0] == 99 && (unsigned char)tb_s100[1] == 111 && (unsigned char)tb_s100[2] == 109 && (unsigned char)tb_s100[3] == 109 && (unsigned char)tb_s100[4] == 97 && (unsigned char)tb_s100[5] == 110 && (unsigned char)tb_s100[6] == 100 && tb_s100[7] == 0); })) {
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
    if (({ const char *tb_s101 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 74))->q_kind; !((unsigned char)tb_s101[0] == 110 && (unsigned char)tb_s101[1] == 101 && (unsigned char)tb_s101[2] == 119 && (unsigned char)tb_s101[3] == 108 && (unsigned char)tb_s101[4] == 105 && (unsigned char)tb_s101[5] == 110 && (unsigned char)tb_s101[6] == 101 && tb_s101[7] == 0); })) {
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
        { const char * tb_v102 = "dict"; const char * *tb_p102 = &(((tb_o_Node *)tb_nn(v_t, 86))->q_name); *tb_p102 = tb_v102; }
        f_parser__next(v_c);
        tb_o_Node * v_key = f_parser__parse_type(v_c);
        if ((({ const char *tb_s103 = ((tb_o_Node *)tb_nn(v_key, 89))->q_name; !((unsigned char)tb_s103[0] == 116 && (unsigned char)tb_s103[1] == 101 && (unsigned char)tb_s103[2] == 120 && (unsigned char)tb_s103[3] == 116 && tb_s103[4] == 0); }) && ({ const char *tb_s104 = ((tb_o_Node *)tb_nn(v_key, 89))->q_name; !((unsigned char)tb_s104[0] == 105 && (unsigned char)tb_s104[1] == 110 && (unsigned char)tb_s104[2] == 116 && tb_s104[3] == 0); }))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_key, 90))->q_line, "ключ у dict бывает text или int", ((tb_o_Node *)tb_nn(v_key, 90))->q_name);
        }
        f_parser__add_kid(v_t, v_key);
        if (({ const char *tb_s105 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 92))->q_kind; ((unsigned char)tb_s105[0] == 110 && (unsigned char)tb_s105[1] == 101 && (unsigned char)tb_s105[2] == 119 && (unsigned char)tb_s105[3] == 108 && (unsigned char)tb_s105[4] == 105 && (unsigned char)tb_s105[5] == 110 && (unsigned char)tb_s105[6] == 101 && tb_s105[7] == 0); })) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после ключа напиши тип значения: dict text int", "");
        }
        f_parser__add_kid(v_t, f_parser__parse_type(v_c));
        return v_t;
    }
    if (f_parser__at_type_word(v_c)) {
        { const char * tb_v106 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 97))->q_word; const char * *tb_p106 = &(((tb_o_Node *)tb_nn(v_t, 97))->q_name); *tb_p106 = tb_v106; }
        f_parser__next(v_c);
        if (({ const char *tb_s107 = ((tb_o_Node *)tb_nn(v_t, 99))->q_name; ((unsigned char)tb_s107[0] == 108 && (unsigned char)tb_s107[1] == 105 && (unsigned char)tb_s107[2] == 115 && (unsigned char)tb_s107[3] == 116 && tb_s107[4] == 0); })) {
            if (((!f_parser__at_type_word(v_c)) && ({ const char *tb_s108 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 100))->q_kind; !((unsigned char)tb_s108[0] == 110 && (unsigned char)tb_s108[1] == 97 && (unsigned char)tb_s108[2] == 109 && (unsigned char)tb_s108[3] == 101 && tb_s108[4] == 0); }))) {
                f_errors__error_at(v_c, f_parser__line_now(v_c), "после list напиши, что в нём лежит: list int, list text ...", "");
            }
            f_parser__add_kid(v_t, f_parser__parse_type(v_c));
        }
        return v_t;
    }
    if (({ const char *tb_s109 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 104))->q_kind; ((unsigned char)tb_s109[0] == 110 && (unsigned char)tb_s109[1] == 97 && (unsigned char)tb_s109[2] == 109 && (unsigned char)tb_s109[3] == 101 && tb_s109[4] == 0); })) {
        { const char * tb_v110 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 105))->q_word; const char * *tb_p110 = &(((tb_o_Node *)tb_nn(v_t, 105))->q_name); *tb_p110 = tb_v110; }
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
    { const char * tb_v111 = v_sign; const char * *tb_p111 = &(((tb_o_Node *)tb_nn(v_n, 115))->q_value); *tb_p111 = tb_v111; }
    f_parser__add_kid(v_n, v_left);
    f_parser__add_kid(v_n, v_right);
    return v_n;
    return v_result;
}

static tb_o_Node * f_parser__parse_call(tb_o_Comp * v_c, const char * v_name, long long v_line) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_call = f_parser__new_node("call", v_line);
    { const char * tb_v112 = v_name; const char * *tb_p112 = &(((tb_o_Node *)tb_nn(v_call, 122))->q_name); *tb_p112 = tb_v112; }
    f_parser__expect_sign(v_c, "(");
    while ((!f_parser__is_sign(v_c, ")"))) {
        tb_o_Node * v_arg = f_parser__new_node("arg", f_parser__line_now(v_c));
        if ((({ const char *tb_s113 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 127))->q_kind; ((unsigned char)tb_s113[0] == 110 && (unsigned char)tb_s113[1] == 97 && (unsigned char)tb_s113[2] == 109 && (unsigned char)tb_s113[3] == 101 && tb_s113[4] == 0); }) && f_parser__ahead_is_sign(v_c, ":"))) {
            { const char * tb_v114 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 128))->q_word; const char * *tb_p114 = &(((tb_o_Node *)tb_nn(v_arg, 128))->q_name); *tb_p114 = tb_v114; }
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
    if (((({ const char *tb_s115 = ((tb_o_Token *)tb_nn(v_t, 143))->q_kind; ((unsigned char)tb_s115[0] == 110 && (unsigned char)tb_s115[1] == 117 && (unsigned char)tb_s115[2] == 109 && (unsigned char)tb_s115[3] == 98 && (unsigned char)tb_s115[4] == 101 && (unsigned char)tb_s115[5] == 114 && tb_s115[6] == 0); }) || ({ const char *tb_s116 = ((tb_o_Token *)tb_nn(v_t, 143))->q_kind; ((unsigned char)tb_s116[0] == 100 && (unsigned char)tb_s116[1] == 101 && (unsigned char)tb_s116[2] == 99 && (unsigned char)tb_s116[3] == 105 && (unsigned char)tb_s116[4] == 109 && (unsigned char)tb_s116[5] == 97 && (unsigned char)tb_s116[6] == 108 && tb_s116[7] == 0); })) || ({ const char *tb_s117 = ((tb_o_Token *)tb_nn(v_t, 143))->q_kind; ((unsigned char)tb_s117[0] == 115 && (unsigned char)tb_s117[1] == 116 && (unsigned char)tb_s117[2] == 114 && (unsigned char)tb_s117[3] == 105 && (unsigned char)tb_s117[4] == 110 && (unsigned char)tb_s117[5] == 103 && tb_s117[6] == 0); }))) {
        v_n = f_parser__new_node(((tb_o_Token *)tb_nn(v_t, 144))->q_kind, v_line);
        { const char * tb_v118 = ((tb_o_Token *)tb_nn(v_t, 145))->q_word; const char * *tb_p118 = &(((tb_o_Node *)tb_nn(v_n, 145))->q_value); *tb_p118 = tb_v118; }
        f_parser__next(v_c);
    } else if ((f_parser__is_cmd(v_c, "true") || f_parser__is_cmd(v_c, "false"))) {
        v_n = f_parser__new_node("bool", v_line);
        { const char * tb_v119 = ((tb_o_Token *)tb_nn(v_t, 149))->q_word; const char * *tb_p119 = &(((tb_o_Node *)tb_nn(v_n, 149))->q_value); *tb_p119 = tb_v119; }
        f_parser__next(v_c);
    } else if (({ const char *tb_s120 = ((tb_o_Token *)tb_nn(v_t, 151))->q_kind; ((unsigned char)tb_s120[0] == 110 && (unsigned char)tb_s120[1] == 97 && (unsigned char)tb_s120[2] == 109 && (unsigned char)tb_s120[3] == 101 && tb_s120[4] == 0); })) {
        f_parser__next(v_c);
        if (f_parser__is_sign(v_c, "(")) {
            v_n = f_parser__parse_call(v_c, ((tb_o_Token *)tb_nn(v_t, 154))->q_word, v_line);
        } else {
            v_n = f_parser__new_node("name", v_line);
            { const char * tb_v121 = ((tb_o_Token *)tb_nn(v_t, 157))->q_word; const char * *tb_p121 = &(((tb_o_Node *)tb_nn(v_n, 157))->q_name); *tb_p121 = tb_v121; }
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
                { const char * tb_v122 = v_name; const char * *tb_p122 = &(((tb_o_Node *)tb_nn(v_m, 201))->q_name); *tb_p122 = tb_v122; }
                f_parser__add_kid(v_m, v_n);
                {
                    tb_list * tb_l123 = ((tb_o_Node *)tb_nn(v_call, 203))->q_kids;
                    for (long long tb_i123 = 0; tb_i123 < tb_l123->len; tb_i123++) {
                        tb_o_Node * const v_k = ((tb_o_Node * *)tb_l123->data)[tb_i123];
                        f_parser__add_kid(v_m, v_k);
                    }
                }
                v_n = v_m;
            } else {
                tb_o_Node * v_f = f_parser__new_node("field", v_dot_line);
                { const char * tb_v124 = v_name; const char * *tb_p124 = &(((tb_o_Node *)tb_nn(v_f, 208))->q_name); *tb_p124 = tb_v124; }
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
        { const char * tb_v125 = "-"; const char * *tb_p125 = &(((tb_o_Node *)tb_nn(v_n, 216))->q_value); *tb_p125 = tb_v125; }
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
        { const char * tb_v126 = "not"; const char * *tb_p126 = &(((tb_o_Node *)tb_nn(v_n, 254))->q_value); *tb_p126 = tb_v126; }
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
    if ((({ const char *tb_s127 = ((tb_o_Token *)tb_nn(v_t, 300))->q_kind; ((unsigned char)tb_s127[0] == 110 && (unsigned char)tb_s127[1] == 97 && (unsigned char)tb_s127[2] == 109 && (unsigned char)tb_s127[3] == 101 && tb_s127[4] == 0); }) && ({ const char *tb_s128 = ((tb_o_Token *)tb_nn(v_t, 300))->q_word; ((unsigned char)tb_s128[0] == 111 && (unsigned char)tb_s128[1] == 110 && tb_s128[2] == 0); }))) {
        f_parser__next(v_c);
        tb_o_Token * v_a = f_parser__now(v_c);
        tb_o_Token * v_b = f_parser__ahead(v_c);
        if ((({ const char *tb_s129 = ((tb_o_Token *)tb_nn(v_a, 304))->q_word; !((unsigned char)tb_s129[0] == 97 && (unsigned char)tb_s129[1] == 108 && (unsigned char)tb_s129[2] == 108 && tb_s129[3] == 0); }) || ({ const char *tb_s130 = ((tb_o_Token *)tb_nn(v_b, 304))->q_word; !((unsigned char)tb_s130[0] == 99 && (unsigned char)tb_s130[1] == 111 && (unsigned char)tb_s130[2] == 114 && (unsigned char)tb_s130[3] == 101 && (unsigned char)tb_s130[4] == 115 && tb_s130[5] == 0); }))) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после on пишется all cores: each x in nums on all cores", "");
        }
        f_parser__next(v_c);
        f_parser__next(v_c);
        { _Bool tb_v131 = 1; _Bool *tb_p131 = &(((tb_o_Node *)tb_nn(v_n, 308))->q_is_par); *tb_p131 = tb_v131; }
    }
}

static tb_o_Node * f_parser__parse_each(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_n = f_parser__new_node("each", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "each");
    { const char * tb_v132 = f_parser__expect_name(v_c); const char * *tb_p132 = &(((tb_o_Node *)tb_nn(v_n, 313))->q_name); *tb_p132 = tb_v132; }
    f_parser__expect_cmd(v_c, "in");
    f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
    f_parser__parse_on_all_cores(v_c, v_n);
    if (f_parser__is_cmd(v_c, "by")) {
        f_parser__next(v_c);
        if ((({ const char *tb_s133 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 321))->q_kind; ((unsigned char)tb_s133[0] == 110 && (unsigned char)tb_s133[1] == 97 && (unsigned char)tb_s133[2] == 109 && (unsigned char)tb_s133[3] == 101 && tb_s133[4] == 0); }) && ({ const char *tb_s134 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 321))->q_word; ((unsigned char)tb_s134[0] == 118 && (unsigned char)tb_s134[1] == 101 && (unsigned char)tb_s134[2] == 99 && tb_s134[3] == 0); }))) {
            f_parser__next(v_c);
            { _Bool tb_v135 = 1; _Bool *tb_p135 = &(((tb_o_Node *)tb_nn(v_n, 323))->q_by_vec); *tb_p135 = tb_v135; }
            if (((tb_o_Node *)tb_nn(v_n, 324))->q_is_par) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 325))->q_line, "on all cores и by vec вместе пока нельзя", "");
            }
        } else {
            { tb_o_Node * tb_v136 = f_parser__parse_expr(v_c); tb_o_Node * *tb_p136 = &(((tb_o_Node *)tb_nn(v_n, 328))->q_piece_size); *tb_p136 = tb_v136; }
            { _Bool tb_v137 = 1; _Bool *tb_p137 = &(((tb_o_Node *)tb_nn(v_n, 329))->q_by_piece); *tb_p137 = tb_v137; }
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
    { const char * tb_v138 = f_parser__expect_name(v_c); const char * *tb_p138 = &(((tb_o_Node *)tb_nn(v_n, 339))->q_name); *tb_p138 = tb_v138; }
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
        if ((({ const char *tb_s139 = ((tb_o_Node *)tb_nn(v_call, 371))->q_kind; !((unsigned char)tb_s139[0] == 99 && (unsigned char)tb_s139[1] == 97 && (unsigned char)tb_s139[2] == 108 && (unsigned char)tb_s139[3] == 108 && tb_s139[4] == 0); }) && ({ const char *tb_s140 = ((tb_o_Node *)tb_nn(v_call, 371))->q_kind; !((unsigned char)tb_s140[0] == 109 && (unsigned char)tb_s140[1] == 101 && (unsigned char)tb_s140[2] == 116 && (unsigned char)tb_s140[3] == 104 && (unsigned char)tb_s140[4] == 111 && (unsigned char)tb_s140[5] == 100 && tb_s140[6] == 0); }))) {
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
        { const char * tb_v141 = f_parser__expect_name(v_c); const char * *tb_p141 = &(((tb_o_Node *)tb_nn(v_e, 384))->q_name); *tb_p141 = tb_v141; }
        f_parser__expect_newline(v_c);
        f_parser__add_kid(v_e, v_v);
        f_parser__add_kid(v_e, f_parser__parse_block(v_c));
        return v_e;
    }
    return v_v;
    return v_result;
}

static void f_parser__end_statement(tb_o_Comp * v_c, tb_o_Node * v_value) {
    if (({ const char *tb_s142 = ((tb_o_Node *)tb_nn(v_value, 393))->q_kind; !((unsigned char)tb_s142[0] == 101 && (unsigned char)tb_s142[1] == 108 && (unsigned char)tb_s142[2] == 115 && (unsigned char)tb_s142[3] == 101 && (unsigned char)tb_s142[4] == 95 && (unsigned char)tb_s142[5] == 101 && (unsigned char)tb_s142[6] == 114 && (unsigned char)tb_s142[7] == 114 && tb_s142[8] == 0); })) {
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
    { const char * tb_v143 = f_parser__expect_name(v_c); const char * *tb_p143 = &(((tb_o_Node *)tb_nn(v_n, 403))->q_name); *tb_p143 = tb_v143; }
    { _Bool tb_v144 = v_is_const; _Bool *tb_p144 = &(((tb_o_Node *)tb_nn(v_n, 404))->q_is_const); *tb_p144 = tb_v144; }
    f_parser__expect_sign(v_c, "=");
    if ((f_parser__at_type_word(v_c) || f_parser__is_cmd(v_c, "dict"))) {
        { tb_o_Node * tb_v145 = f_parser__parse_type(v_c); tb_o_Node * *tb_p145 = &(((tb_o_Node *)tb_nn(v_n, 407))->q_tsyn); *tb_p145 = tb_v145; }
        { _Bool tb_v146 = 1; _Bool *tb_p146 = &(((tb_o_Node *)tb_nn(v_n, 408))->q_has_tsyn); *tb_p146 = tb_v146; }
    }
    if (({ const char *tb_s147 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 409))->q_kind; !((unsigned char)tb_s147[0] == 110 && (unsigned char)tb_s147[1] == 101 && (unsigned char)tb_s147[2] == 119 && (unsigned char)tb_s147[3] == 108 && (unsigned char)tb_s147[4] == 105 && (unsigned char)tb_s147[5] == 110 && (unsigned char)tb_s147[6] == 101 && tb_s147[7] == 0); })) {
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
    { const char * tb_v148 = f_parser__expect_name(v_c); const char * *tb_p148 = &(((tb_o_Node *)tb_nn(v_n, 424))->q_name); *tb_p148 = tb_v148; }
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
        { const char * tb_v149 = f_parser__expect_name(v_c); const char * *tb_p149 = &(((tb_o_Node *)tb_nn(v_name, 438))->q_name); *tb_p149 = tb_v149; }
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
        if (((({ const char *tb_s150 = v_k; !((unsigned char)tb_s150[0] == 110 && (unsigned char)tb_s150[1] == 97 && (unsigned char)tb_s150[2] == 109 && (unsigned char)tb_s150[3] == 101 && tb_s150[4] == 0); }) && ({ const char *tb_s151 = v_k; !((unsigned char)tb_s151[0] == 105 && (unsigned char)tb_s151[1] == 110 && (unsigned char)tb_s151[2] == 100 && (unsigned char)tb_s151[3] == 101 && (unsigned char)tb_s151[4] == 120 && tb_s151[5] == 0); })) && ({ const char *tb_s152 = v_k; !((unsigned char)tb_s152[0] == 102 && (unsigned char)tb_s152[1] == 105 && (unsigned char)tb_s152[2] == 101 && (unsigned char)tb_s152[3] == 108 && (unsigned char)tb_s152[4] == 100 && tb_s152[5] == 0); }))) {
            f_errors__error_at(v_c, v_line, "слева от знака должно быть имя, nums[i] или u.age", "");
        }
        tb_o_Node * v_n = f_parser__new_node("set", v_line);
        { const char * tb_v153 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 464))->q_word; const char * *tb_p153 = &(((tb_o_Node *)tb_nn(v_n, 464))->q_value); *tb_p153 = tb_v153; }
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
        { const char * tb_v154 = f_parser__expect_name(v_c); const char * *tb_p154 = &(((tb_o_Node *)tb_nn(v_e, 475))->q_name); *tb_p154 = tb_v154; }
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
        tb_list * tb_l155 = tb_list_from(4, sizeof(const char *), (const char *[]){"gives", "obj", "danger", "by"});
        for (long long tb_i155 = 0; tb_i155 < tb_l155->len; tb_i155++) {
            const char * const v_w = ((const char * *)tb_l155->data)[tb_i155];
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
        if (({ const char *tb_s156 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 509))->q_kind; !((unsigned char)tb_s156[0] == 110 && (unsigned char)tb_s156[1] == 101 && (unsigned char)tb_s156[2] == 119 && (unsigned char)tb_s156[3] == 108 && (unsigned char)tb_s156[4] == 105 && (unsigned char)tb_s156[5] == 110 && (unsigned char)tb_s156[6] == 101 && tb_s156[7] == 0); })) {
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
        if ((({ const char *tb_s157 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 547))->q_kind; !((unsigned char)tb_s157[0] == 110 && (unsigned char)tb_s157[1] == 97 && (unsigned char)tb_s157[2] == 109 && (unsigned char)tb_s157[3] == 101 && tb_s157[4] == 0); }) || (!f_parser__ahead_is_sign(v_c, "=")))) {
            f_errors__error_at(v_c, v_line, "после shared пишется новая переменная: shared y = int 0", "");
        }
        tb_o_Node * v_n = f_parser__parse_assign(v_c, 0);
        { _Bool tb_v158 = 1; _Bool *tb_p158 = &(((tb_o_Node *)tb_nn(v_n, 550))->q_is_shared); *tb_p158 = tb_v158; }
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
    if (({ const char *tb_s159 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 562))->q_kind; ((unsigned char)tb_s159[0] == 110 && (unsigned char)tb_s159[1] == 97 && (unsigned char)tb_s159[2] == 109 && (unsigned char)tb_s159[3] == 101 && tb_s159[4] == 0); })) {
        return f_parser__parse_name_statement(v_c);
    }
    if ((({ const char *tb_s160 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 564))->q_kind; ((unsigned char)tb_s160[0] == 99 && (unsigned char)tb_s160[1] == 111 && (unsigned char)tb_s160[2] == 109 && (unsigned char)tb_s160[3] == 109 && (unsigned char)tb_s160[4] == 97 && (unsigned char)tb_s160[5] == 110 && (unsigned char)tb_s160[6] == 100 && tb_s160[7] == 0); }) && f_parser__ahead_is_sign(v_c, "="))) {
        f_errors__error_at(v_c, v_line, "это слово занято языком, возьми другое имя", ((tb_o_Token *)tb_nn(f_parser__now(v_c), 565))->q_word);
    }
    f_errors__error_at(v_c, v_line, "не понимаю, что делает строка, начиная с", f_parser__what_is_here(v_c));
    return f_parser__new_node("missing", v_line);
    return v_result;
}

static tb_o_Node * f_parser__parse_block(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_block = f_parser__new_node("block", f_parser__line_now(v_c));
    if (({ const char *tb_s161 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 571))->q_kind; !((unsigned char)tb_s161[0] == 105 && (unsigned char)tb_s161[1] == 110 && (unsigned char)tb_s161[2] == 100 && (unsigned char)tb_s161[3] == 101 && (unsigned char)tb_s161[4] == 110 && (unsigned char)tb_s161[5] == 116 && tb_s161[6] == 0); })) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "после этой строки нужен блок с отступом", "");
    }
    f_parser__next(v_c);
    while ((({ const char *tb_s162 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 574))->q_kind; !((unsigned char)tb_s162[0] == 100 && (unsigned char)tb_s162[1] == 101 && (unsigned char)tb_s162[2] == 100 && (unsigned char)tb_s162[3] == 101 && (unsigned char)tb_s162[4] == 110 && (unsigned char)tb_s162[5] == 116 && tb_s162[6] == 0); }) && ({ const char *tb_s163 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 574))->q_kind; !((unsigned char)tb_s163[0] == 101 && (unsigned char)tb_s163[1] == 110 && (unsigned char)tb_s163[2] == 100 && tb_s163[3] == 0); }))) {
        if (({ const char *tb_s164 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 575))->q_kind; ((unsigned char)tb_s164[0] == 105 && (unsigned char)tb_s164[1] == 110 && (unsigned char)tb_s164[2] == 100 && (unsigned char)tb_s164[3] == 101 && (unsigned char)tb_s164[4] == 110 && (unsigned char)tb_s164[5] == 116 && tb_s164[6] == 0); })) {
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
        { _Bool tb_v165 = 1; _Bool *tb_p165 = &(((tb_o_Node *)tb_nn(v_p, 588))->q_is_link); *tb_p165 = tb_v165; }
        { const char * tb_v166 = f_parser__expect_name(v_c); const char * *tb_p166 = &(((tb_o_Node *)tb_nn(v_p, 589))->q_name); *tb_p166 = tb_v166; }
        return v_p;
    }
    { const char * tb_v167 = f_parser__expect_name(v_c); const char * *tb_p167 = &(((tb_o_Node *)tb_nn(v_p, 591))->q_name); *tb_p167 = tb_v167; }
    f_parser__expect_sign(v_c, ":");
    { tb_o_Node * tb_v168 = f_parser__parse_type(v_c); tb_o_Node * *tb_p168 = &(((tb_o_Node *)tb_nn(v_p, 593))->q_tsyn); *tb_p168 = tb_v168; }
    { _Bool tb_v169 = 1; _Bool *tb_p169 = &(((tb_o_Node *)tb_nn(v_p, 594))->q_has_tsyn); *tb_p169 = tb_v169; }
    return v_p;
    return v_result;
}

static tb_o_Node * f_parser__parse_func(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_f = f_parser__new_node("func", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "func");
    { const char * tb_v170 = f_parser__expect_name(v_c); const char * *tb_p170 = &(((tb_o_Node *)tb_nn(v_f, 601))->q_name); *tb_p170 = tb_v170; }
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
    { const char * tb_v171 = f_parser__expect_name(v_c); const char * *tb_p171 = &(((tb_o_Node *)tb_nn(v_o, 630))->q_name); *tb_p171 = tb_v171; }
    if (f_parser__is_cmd(v_c, "by")) {
        f_parser__next(v_c);
        if ((({ const char *tb_s172 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 634))->q_kind; !((unsigned char)tb_s172[0] == 110 && (unsigned char)tb_s172[1] == 97 && (unsigned char)tb_s172[2] == 109 && (unsigned char)tb_s172[3] == 101 && tb_s172[4] == 0); }) || ({ const char *tb_s173 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 634))->q_word; !((unsigned char)tb_s173[0] == 99 && (unsigned char)tb_s173[1] == 111 && (unsigned char)tb_s173[2] == 108 && (unsigned char)tb_s173[3] == 117 && (unsigned char)tb_s173[4] == 109 && (unsigned char)tb_s173[5] == 110 && (unsigned char)tb_s173[6] == 115 && tb_s173[7] == 0); }))) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после obj ... by пишется columns: obj Point by columns", "");
        }
        f_parser__next(v_c);
        { _Bool tb_v174 = 1; _Bool *tb_p174 = &(((tb_o_Node *)tb_nn(v_o, 637))->q_by_cols); *tb_p174 = tb_v174; }
    }
    f_parser__expect_newline(v_c);
    if (({ const char *tb_s175 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 639))->q_kind; !((unsigned char)tb_s175[0] == 105 && (unsigned char)tb_s175[1] == 110 && (unsigned char)tb_s175[2] == 100 && (unsigned char)tb_s175[3] == 101 && (unsigned char)tb_s175[4] == 110 && (unsigned char)tb_s175[5] == 116 && tb_s175[6] == 0); })) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "у obj ниже с отступом пишутся качества: name: text", "");
    }
    f_parser__next(v_c);
    while ((({ const char *tb_s176 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 642))->q_kind; !((unsigned char)tb_s176[0] == 100 && (unsigned char)tb_s176[1] == 101 && (unsigned char)tb_s176[2] == 100 && (unsigned char)tb_s176[3] == 101 && (unsigned char)tb_s176[4] == 110 && (unsigned char)tb_s176[5] == 116 && tb_s176[6] == 0); }) && ({ const char *tb_s177 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 642))->q_kind; !((unsigned char)tb_s177[0] == 101 && (unsigned char)tb_s177[1] == 110 && (unsigned char)tb_s177[2] == 100 && tb_s177[3] == 0); }))) {
        f_parser__add_kid(v_o, f_parser__parse_param(v_c, 0));
        f_parser__expect_newline(v_c);
    }
    f_parser__next(v_c);
    return v_o;
    return v_result;
}

static tb_o_Node * f_parser__parse(tb_o_Comp * v_c) {
    { long long tb_v178 = 0LL; long long *tb_p178 = &(((tb_o_Comp *)tb_nn(v_c, 649))->q_pos); *tb_p178 = tb_v178; }
    tb_o_Node * v_program = f_parser__new_node("program", 1LL);
    while (({ const char *tb_s179 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 651))->q_kind; !((unsigned char)tb_s179[0] == 101 && (unsigned char)tb_s179[1] == 110 && (unsigned char)tb_s179[2] == 100 && tb_s179[3] == 0); })) {
        if (f_parser__is_cmd(v_c, "use")) {
            tb_o_Node * v_u = f_parser__new_node("use", f_parser__line_now(v_c));
            f_parser__next(v_c);
            { const char * tb_v180 = f_parser__expect_name(v_c); const char * *tb_p180 = &(((tb_o_Node *)tb_nn(v_u, 656))->q_name); *tb_p180 = tb_v180; }
            f_parser__expect_newline(v_c);
            f_parser__add_kid(v_program, v_u);
        } else if ((f_parser__is_cmd(v_c, "modpub") || f_parser__is_cmd(v_c, "modpri"))) {
            _Bool v_is_pub = f_parser__is_cmd(v_c, "modpub");
            f_parser__next(v_c);
            if ((!f_parser__is_cmd(v_c, "func"))) {
                f_errors__error_at(v_c, f_parser__line_now(v_c), "modpub и modpri ставятся перед func", "");
            }
            tb_o_Node * v_f = f_parser__parse_func(v_c);
            { _Bool tb_v181 = v_is_pub; _Bool *tb_p181 = &(((tb_o_Node *)tb_nn(v_f, 665))->q_is_pub); *tb_p181 = tb_v181; }
            f_parser__add_kid(v_program, v_f);
        } else if (f_parser__is_cmd(v_c, "func")) {
            f_parser__add_kid(v_program, f_parser__parse_func(v_c));
        } else if (f_parser__is_cmd(v_c, "obj")) {
            f_parser__add_kid(v_program, f_parser__parse_obj(v_c));
        } else if (f_parser__is_cmd(v_c, "link")) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "подключение C-файлов через link будет в версии 0.9 (вместе с danger); файлы Tachbit подключает use", "");
        } else if (({ const char *tb_s182 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 673))->q_kind; ((unsigned char)tb_s182[0] == 105 && (unsigned char)tb_s182[1] == 110 && (unsigned char)tb_s182[2] == 100 && (unsigned char)tb_s182[3] == 101 && (unsigned char)tb_s182[4] == 110 && (unsigned char)tb_s182[5] == 116 && tb_s182[6] == 0); })) {
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
        long long tb_end183 = v_depth;
        for (long long v_i = 1LL; v_i <= tb_end183; v_i++) {
            tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, "  ", sizeof("  ") - 1); 
        }
    }
    tb_app(&v_s, &tb_rl_s, &tb_rc_s, f_parser__node_kind_name(((tb_o_Node *)tb_nn(v_n, 689))->q_kind)); 
    if (({ const char *tb_s184 = ((tb_o_Node *)tb_nn(v_n, 690))->q_name; !(tb_s184[0] == 0); })) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, " ", sizeof(" ") - 1); tb_app(&v_s, &tb_rl_s, &tb_rc_s, ((tb_o_Node *)tb_nn(v_n, 691))->q_name); 
    }
    if (({ const char *tb_s185 = ((tb_o_Node *)tb_nn(v_n, 692))->q_value; !(tb_s185[0] == 0); })) {
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
    { const char * tb_v186 = (tb_rl_s = -1, v_s); *(const char * *)tb_grow(v_out) = tb_v186; }
    if (((tb_o_Node *)tb_nn(v_n, 707))->q_has_tsyn) {
        f_parser__tree_lines(((tb_o_Node *)tb_nn(v_n, 708))->q_tsyn, (v_depth + 1LL), v_out);
    }
    {
        tb_list * tb_l187 = ((tb_o_Node *)tb_nn(v_n, 709))->q_kids;
        for (long long tb_i187 = 0; tb_i187 < tb_l187->len; tb_i187++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l187->data)[tb_i187];
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
        v_func_key = tb_cat(tb_cat(((tb_o_Node *)tb_nn(v_fnode, 17))->q_mod, "."), ((tb_o_Node *)tb_nn(v_fnode, 17))->q_name);
    }
    {
        tb_list * tb_l188 = ((tb_o_Comp *)tb_nn(v_c, 18))->q_types;
        for (long long tb_i188 = 0; tb_i188 < tb_l188->len; tb_i188++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l188->data)[tb_i188];
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
        { tb_o_Node * tb_v189 = v_onode; tb_o_Node * *tb_p189 = &(((tb_o_Type *)tb_nn(v_t, 25))->q_obj_node); *tb_p189 = tb_v189; }
    }
    if (v_has_func) {
        { tb_o_Node * tb_v190 = v_fnode; tb_o_Node * *tb_p190 = &(((tb_o_Type *)tb_nn(v_t, 27))->q_func_node); *tb_p190 = tb_v190; }
    }
    { tb_o_Type * tb_v191 = v_t; *(tb_o_Type * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 28))->q_types) = tb_v191; }
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
    if (({ const char *tb_s192 = ((tb_o_Type *)tb_nn(v_ty, 65))->q_kind; ((unsigned char)tb_s192[0] == 108 && (unsigned char)tb_s192[1] == 105 && (unsigned char)tb_s192[2] == 115 && (unsigned char)tb_s192[3] == 116 && tb_s192[4] == 0); })) {
        return tb_cat("list ", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 66))->q_elem));
    }
    if (({ const char *tb_s193 = ((tb_o_Type *)tb_nn(v_ty, 67))->q_kind; ((unsigned char)tb_s193[0] == 100 && (unsigned char)tb_s193[1] == 105 && (unsigned char)tb_s193[2] == 99 && (unsigned char)tb_s193[3] == 116 && tb_s193[4] == 0); })) {
        return tb_cat(tb_cat(tb_cat("dict ", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 68))->q_key)), " "), f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 68))->q_elem));
    }
    if (({ const char *tb_s194 = ((tb_o_Type *)tb_nn(v_ty, 69))->q_kind; ((unsigned char)tb_s194[0] == 111 && (unsigned char)tb_s194[1] == 98 && (unsigned char)tb_s194[2] == 106 && tb_s194[3] == 0); })) {
        return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn(v_ty, 70))->q_obj_node, 70))->q_name;
    }
    if (({ const char *tb_s195 = ((tb_o_Type *)tb_nn(v_ty, 71))->q_kind; ((unsigned char)tb_s195[0] == 109 && (unsigned char)tb_s195[1] == 117 && (unsigned char)tb_s195[2] == 108 && (unsigned char)tb_s195[3] == 116 && (unsigned char)tb_s195[4] == 105 && tb_s195[5] == 0); })) {
        return "несколько выходов";
    }
    if (({ const char *tb_s196 = ((tb_o_Type *)tb_nn(v_ty, 73))->q_kind; ((unsigned char)tb_s196[0] == 110 && (unsigned char)tb_s196[1] == 111 && (unsigned char)tb_s196[2] == 110 && (unsigned char)tb_s196[3] == 101 && tb_s196[4] == 0); })) {
        return "ничего";
    }
    if (({ const char *tb_s197 = ((tb_o_Type *)tb_nn(v_ty, 75))->q_kind; ((unsigned char)tb_s197[0] == 116 && (unsigned char)tb_s197[1] == 97 && (unsigned char)tb_s197[2] == 115 && (unsigned char)tb_s197[3] == 107 && tb_s197[4] == 0); })) {
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
    return ({ const char *tb_s198 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s198[0] == 108 && (unsigned char)tb_s198[1] == 105 && (unsigned char)tb_s198[2] == 115 && (unsigned char)tb_s198[3] == 116 && tb_s198[4] == 0); });
    tb_crash(82, "функция is_list дошла до конца без return");
    return 0;
}

static _Bool f_check__is_dict(tb_o_Comp * v_c, long long v_t) {
    return ({ const char *tb_s199 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s199[0] == 100 && (unsigned char)tb_s199[1] == 105 && (unsigned char)tb_s199[2] == 99 && (unsigned char)tb_s199[3] == 116 && tb_s199[4] == 0); });
    tb_crash(85, "функция is_dict дошла до конца без return");
    return 0;
}

static _Bool f_check__is_cols_obj(tb_o_Comp * v_c, long long v_t) {
    return (({ const char *tb_s200 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s200[0] == 111 && (unsigned char)tb_s200[1] == 98 && (unsigned char)tb_s200[2] == 106 && tb_s200[3] == 0); }) && ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 90))->q_types, v_t, sizeof(tb_o_Type *), 90)), 90))->q_obj_node, 90))->q_by_cols);
    tb_crash(89, "функция is_cols_obj дошла до конца без return");
    return 0;
}

static _Bool f_check__is_cols_list(tb_o_Comp * v_c, long long v_t) {
    return (({ const char *tb_s201 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s201[0] == 108 && (unsigned char)tb_s201[1] == 105 && (unsigned char)tb_s201[2] == 115 && (unsigned char)tb_s201[3] == 116 && tb_s201[4] == 0); }) && f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 94))->q_types, v_t, sizeof(tb_o_Type *), 94)), 94))->q_elem));
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
        tb_list * tb_l202 = tb_list_from(29, sizeof(const char *), (const char *[]){"print", "len", "round", "code", "char", "cut", "to_int", "to_dec", "read_file", "write_file", "input", "args", "split", "join", "find", "replace", "upper", "lower", "trim", "starts_with", "ends_with", "sort", "abs", "min", "max", "sqrt", "run", "exit", "file_exists"});
        for (long long tb_i202 = 0; tb_i202 < tb_l202->len; tb_i202++) {
            const char * const v_b = ((const char * *)tb_l202->data)[tb_i202];
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
    return ((((({ const char *tb_s203 = v_name; ((unsigned char)tb_s203[0] == 116 && (unsigned char)tb_s203[1] == 111 && (unsigned char)tb_s203[2] == 95 && (unsigned char)tb_s203[3] == 105 && (unsigned char)tb_s203[4] == 110 && (unsigned char)tb_s203[5] == 116 && tb_s203[6] == 0); }) || ({ const char *tb_s204 = v_name; ((unsigned char)tb_s204[0] == 116 && (unsigned char)tb_s204[1] == 111 && (unsigned char)tb_s204[2] == 95 && (unsigned char)tb_s204[3] == 100 && (unsigned char)tb_s204[4] == 101 && (unsigned char)tb_s204[5] == 99 && tb_s204[6] == 0); })) || (strcmp(v_name, "read_file") == 0)) || (strcmp(v_name, "write_file") == 0)) || ({ const char *tb_s205 = v_name; ((unsigned char)tb_s205[0] == 105 && (unsigned char)tb_s205[1] == 110 && (unsigned char)tb_s205[2] == 112 && (unsigned char)tb_s205[3] == 117 && (unsigned char)tb_s205[4] == 116 && tb_s205[5] == 0); }));
    tb_crash(116, "функция builtin_can_fail дошла до конца без return");
    return 0;
}

static _Bool f_check__is_module(tb_o_Comp * v_c, const char * v_name) {
    {
        tb_list * tb_l206 = ((tb_o_Comp *)tb_nn(v_c, 123))->q_all_mods;
        for (long long tb_i206 = 0; tb_i206 < tb_l206->len; tb_i206++) {
            const char * const v_m = ((const char * *)tb_l206->data)[tb_i206];
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
        tb_list * tb_l207 = ((tb_o_Comp *)tb_nn(v_c, 131))->q_uses;
        for (long long tb_i207 = 0; tb_i207 < tb_l207->len; tb_i207++) {
            tb_o_Use * const v_u = ((tb_o_Use * *)tb_l207->data)[tb_i207];
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
        long long tb_end208 = ((((tb_o_Comp *)tb_nn(v_c, 138))->q_funcs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end208; v_i++) {
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
        long long tb_end209 = ((((tb_o_Comp *)tb_nn(v_c, 150))->q_funcs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end209; v_i++) {
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
        tb_list * tb_l210 = ((tb_o_Comp *)tb_nn(v_c, 160))->q_funcs;
        for (long long tb_i210 = 0; tb_i210 < tb_l210->len; tb_i210++) {
            tb_o_Func * const v_f = ((tb_o_Func * *)tb_l210->data)[tb_i210];
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
        long long tb_end211 = ((((tb_o_Comp *)tb_nn(v_c, 169))->q_objs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end211; v_i++) {
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
        long long tb_end212 = ((((tb_o_Node *)tb_nn(v_o, 175))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end212; v_i++) {
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
    if (({ const char *tb_s213 = ((tb_o_Node *)tb_nn(v_t, 181))->q_name; ((unsigned char)tb_s213[0] == 105 && (unsigned char)tb_s213[1] == 110 && (unsigned char)tb_s213[2] == 116 && tb_s213[3] == 0); })) {
        return 1LL;
    }
    if (({ const char *tb_s214 = ((tb_o_Node *)tb_nn(v_t, 183))->q_name; ((unsigned char)tb_s214[0] == 100 && (unsigned char)tb_s214[1] == 101 && (unsigned char)tb_s214[2] == 99 && tb_s214[3] == 0); })) {
        return 2LL;
    }
    if (({ const char *tb_s215 = ((tb_o_Node *)tb_nn(v_t, 185))->q_name; ((unsigned char)tb_s215[0] == 116 && (unsigned char)tb_s215[1] == 101 && (unsigned char)tb_s215[2] == 120 && (unsigned char)tb_s215[3] == 116 && tb_s215[4] == 0); })) {
        return 4LL;
    }
    if (({ const char *tb_s216 = ((tb_o_Node *)tb_nn(v_t, 187))->q_name; ((unsigned char)tb_s216[0] == 98 && (unsigned char)tb_s216[1] == 111 && (unsigned char)tb_s216[2] == 111 && (unsigned char)tb_s216[3] == 108 && tb_s216[4] == 0); })) {
        return 3LL;
    }
    if (({ const char *tb_s217 = ((tb_o_Node *)tb_nn(v_t, 189))->q_name; ((unsigned char)tb_s217[0] == 108 && (unsigned char)tb_s217[1] == 105 && (unsigned char)tb_s217[2] == 115 && (unsigned char)tb_s217[3] == 116 && tb_s217[4] == 0); })) {
        return f_check__list_of(v_c, f_check__resolve_type(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 190))->q_kids, 0LL, sizeof(tb_o_Node *), 190))));
    }
    if (({ const char *tb_s218 = ((tb_o_Node *)tb_nn(v_t, 191))->q_name; ((unsigned char)tb_s218[0] == 100 && (unsigned char)tb_s218[1] == 105 && (unsigned char)tb_s218[2] == 99 && (unsigned char)tb_s218[3] == 116 && tb_s218[4] == 0); })) {
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
        { long long tb_v219 = 1LL; long long *tb_p219 = &(v_i); *tb_p219 -= tb_v219; }
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
    { tb_o_Var * tb_v220 = tb_new_Var(v_name, v_type, v_is_const, v_is_link, ((tb_o_Comp *)tb_nn(v_c, 224))->q_depth, 0, ""); *(tb_o_Var * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 224))->q_vars) = tb_v220; }
}

static void f_check__cut_vars(tb_o_Comp * v_c, long long v_count_left) {
    while (((((tb_o_Comp *)tb_nn(v_c, 227))->q_vars)->len > v_count_left)) {
        tb_o_Var * v_gone = (*(tb_o_Var * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 228))->q_vars, 228));
    }
}

static void f_check__open_scope(tb_o_Comp * v_c) {
    { long long tb_v221 = 1LL; long long *tb_p221 = &(((tb_o_Comp *)tb_nn(v_c, 231))->q_depth); *tb_p221 += tb_v221; }
}

static void f_check__close_scope(tb_o_Comp * v_c) {
    while ((((((tb_o_Comp *)tb_nn(v_c, 234))->q_vars)->len > 0LL) && (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 234))->q_vars, ((((tb_o_Comp *)tb_nn(v_c, 234))->q_vars)->len - 1LL), sizeof(tb_o_Var *), 234)), 234))->q_depth == ((tb_o_Comp *)tb_nn(v_c, 234))->q_depth))) {
        tb_o_Var * v_gone = (*(tb_o_Var * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 235))->q_vars, 235));
    }
    { long long tb_v222 = 1LL; long long *tb_p222 = &(((tb_o_Comp *)tb_nn(v_c, 236))->q_depth); *tb_p222 -= tb_v222; }
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
    if (((({ const char *tb_s223 = ((tb_o_Node *)tb_nn(v_n, 251))->q_kind; ((unsigned char)tb_s223[0] == 105 && (unsigned char)tb_s223[1] == 110 && (unsigned char)tb_s223[2] == 100 && (unsigned char)tb_s223[3] == 101 && (unsigned char)tb_s223[4] == 120 && tb_s223[5] == 0); }) || ({ const char *tb_s224 = ((tb_o_Node *)tb_nn(v_n, 251))->q_kind; ((unsigned char)tb_s224[0] == 102 && (unsigned char)tb_s224[1] == 105 && (unsigned char)tb_s224[2] == 101 && (unsigned char)tb_s224[3] == 108 && (unsigned char)tb_s224[4] == 100 && tb_s224[5] == 0); })) || ({ const char *tb_s225 = ((tb_o_Node *)tb_nn(v_n, 251))->q_kind; ((unsigned char)tb_s225[0] == 109 && (unsigned char)tb_s225[1] == 101 && (unsigned char)tb_s225[2] == 116 && (unsigned char)tb_s225[3] == 104 && (unsigned char)tb_s225[4] == 111 && (unsigned char)tb_s225[5] == 100 && tb_s225[6] == 0); }))) {
        return f_check__root_var(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 252))->q_kids, 0LL, sizeof(tb_o_Node *), 252)));
    }
    if (({ const char *tb_s226 = ((tb_o_Node *)tb_nn(v_n, 253))->q_kind; ((unsigned char)tb_s226[0] == 110 && (unsigned char)tb_s226[1] == 97 && (unsigned char)tb_s226[2] == 109 && (unsigned char)tb_s226[3] == 101 && tb_s226[4] == 0); })) {
        return f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 254))->q_name);
    }
    return (-1LL);
    return v_r;
}

static void f_check__capture(tb_o_Comp * v_c, long long v_v) {
    tb_o_Var * v_var = (*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 259))->q_vars, v_v, sizeof(tb_o_Var *), 259));
    {
        tb_list * tb_l227 = ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 260))->q_par_node, 260))->q_caps;
        for (long long tb_i227 = 0; tb_i227 < tb_l227->len; tb_i227++) {
            tb_o_Var * const v_old = ((tb_o_Var * *)tb_l227->data)[tb_i227];
            if (((strcmp(((tb_o_Var *)tb_nn(v_old, 261))->q_name, ((tb_o_Var *)tb_nn(v_var, 261))->q_name) == 0) && ((!((tb_o_Comp *)tb_nn(v_c, 261))->q_par_vec) || (!((tb_o_Var *)tb_nn(v_old, 261))->q_is_shared)))) {
                return;
            }
        }
    }
    { tb_o_Var * tb_v228 = tb_new_Var(((tb_o_Var *)tb_nn(v_var, 263))->q_name, ((tb_o_Var *)tb_nn(v_var, 263))->q_type, 0, ((tb_o_Var *)tb_nn(v_var, 263))->q_is_link, 0LL, (((tb_o_Var *)tb_nn(v_var, 263))->q_is_shared && (!((tb_o_Comp *)tb_nn(v_c, 263))->q_par_vec)), ""); *(tb_o_Var * *)tb_grow(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 263))->q_par_node, 263))->q_caps) = tb_v228; }
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
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 288))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)"), "");
    }
}

static void f_check__need_value(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t) {
    if ((v_t == 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 294))->q_line, "это ничего не возвращает, значение нельзя использовать", ((tb_o_Node *)tb_nn(v_n, 294))->q_name);
    }
    if (({ const char *tb_s229 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s229[0] == 109 && (unsigned char)tb_s229[1] == 117 && (unsigned char)tb_s229[2] == 108 && (unsigned char)tb_s229[3] == 116 && (unsigned char)tb_s229[4] == 105 && tb_s229[5] == 0); })) {
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
    if ((({ const char *tb_s230 = v_op; ((unsigned char)tb_s230[0] == 111 && (unsigned char)tb_s230[1] == 114 && tb_s230[2] == 0); }) && f_check__can_fail_call(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 305))->q_kids, 0LL, sizeof(tb_o_Node *), 305))))) {
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
        { long long tb_v231 = v_a; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 315))->q_temp_types) = tb_v231; }
        { long long tb_v232 = ((((tb_o_Comp *)tb_nn(v_c, 316))->q_temp_types)->len - 1LL); long long *tb_p232 = &(((tb_o_Node *)tb_nn(v_n, 316))->q_temp_no); *tb_p232 = tb_v232; }
        { const char * tb_v233 = "orelse"; const char * *tb_p233 = &(((tb_o_Node *)tb_nn(v_n, 317))->q_value); *tb_p233 = tb_v233; }
        return v_a;
    }
    long long v_a = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 319))->q_kids, 0LL, sizeof(tb_o_Node *), 319)));
    long long v_b = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 320))->q_kids, 1LL, sizeof(tb_o_Node *), 320)));
    if ((({ const char *tb_s234 = v_op; ((unsigned char)tb_s234[0] == 97 && (unsigned char)tb_s234[1] == 110 && (unsigned char)tb_s234[2] == 100 && tb_s234[3] == 0); }) || ({ const char *tb_s235 = v_op; ((unsigned char)tb_s235[0] == 111 && (unsigned char)tb_s235[1] == 114 && tb_s235[2] == 0); }))) {
        if (((v_a != 3LL) || (v_b != 3LL))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 323))->q_line, "and / or работают только с true и false", "");
        }
        return 3LL;
    }
    if ((({ const char *tb_s236 = v_op; ((unsigned char)tb_s236[0] == 61 && (unsigned char)tb_s236[1] == 61 && tb_s236[2] == 0); }) || ({ const char *tb_s237 = v_op; ((unsigned char)tb_s237[0] == 33 && (unsigned char)tb_s237[1] == 61 && tb_s237[2] == 0); }))) {
        if (((!(f_check__is_number(v_a) && f_check__is_number(v_b))) && (v_a != v_b))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 327))->q_line, "нельзя сравнить разные типы", "");
        }
        if ((!f_check__is_scalar(v_a))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 329))->q_line, "списки и объекты пока нельзя сравнивать", "");
        }
        return 3LL;
    }
    if ((((({ const char *tb_s238 = v_op; ((unsigned char)tb_s238[0] == 60 && tb_s238[1] == 0); }) || ({ const char *tb_s239 = v_op; ((unsigned char)tb_s239[0] == 62 && tb_s239[1] == 0); })) || ({ const char *tb_s240 = v_op; ((unsigned char)tb_s240[0] == 60 && (unsigned char)tb_s240[1] == 61 && tb_s240[2] == 0); })) || ({ const char *tb_s241 = v_op; ((unsigned char)tb_s241[0] == 62 && (unsigned char)tb_s241[1] == 61 && tb_s241[2] == 0); }))) {
        if (((v_a == 4LL) && (v_b == 4LL))) {
            return 3LL;
        }
        if (((!f_check__is_number(v_a)) || (!f_check__is_number(v_b)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 335))->q_line, "больше / меньше работают с числами или с двумя текстами", v_op);
        }
        return 3LL;
    }
    if ((({ const char *tb_s242 = v_op; ((unsigned char)tb_s242[0] == 43 && tb_s242[1] == 0); }) && ((v_a == 4LL) || (v_b == 4LL)))) {
        if (((!f_check__is_scalar(v_a)) || (!f_check__is_scalar(v_b)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 339))->q_line, "список или объект нельзя сложить с текстом", "");
        }
        return 4LL;
    }
    if (((!f_check__is_number(v_a)) || (!f_check__is_number(v_b)))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 342))->q_line, "эта операция работает только с числами", v_op);
    }
    if ((({ const char *tb_s243 = v_op; ((unsigned char)tb_s243[0] == 37 && tb_s243[1] == 0); }) && ((v_a == 2LL) || (v_b == 2LL)))) {
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
        long long tb_end244 = ((((tb_o_Node *)tb_nn(v_n, 353))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end244; v_i++) {
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
    if (({ const char *tb_s245 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 365))->q_kids, v_i, sizeof(tb_o_Node *), 365)), 365))->q_name; !(tb_s245[0] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 366))->q_line, "у встроенных функций аргументы пишутся без имён", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 366))->q_kids, v_i, sizeof(tb_o_Node *), 366)), 366))->q_name);
    }
    long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 367))->q_kids, v_i, sizeof(tb_o_Node *), 367)), 367))->q_kids, 0LL, sizeof(tb_o_Node *), 367)));
    { long long tb_v246 = v_t; long long *tb_p246 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 368))->q_kids, v_i, sizeof(tb_o_Node *), 368)), 368))->q_ty); *tb_p246 = tb_v246; }
    return v_t;
    return v_r;
}

static void f_check__builtin_args(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_pattern, const char * v_usage) {
    if (((((tb_o_Node *)tb_nn(v_n, 374))->q_kids)->len != tb_tlen(v_pattern))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 375))->q_line, "не то количество значений, правильно так", v_usage);
    }
    {
        long long tb_end247 = (tb_tlen(v_pattern) - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end247; v_i++) {
            long long v_t = f_check__builtin_arg(v_c, v_n, v_i);
            const char * v_p = tb_char_at(v_pattern, v_i, -1, 378);
            _Bool v_ok = 1;
            if (({ const char *tb_s248 = v_p; ((unsigned char)tb_s248[0] == 116 && tb_s248[1] == 0); })) {
                v_ok = (v_t == 4LL);
            } else if (({ const char *tb_s249 = v_p; ((unsigned char)tb_s249[0] == 105 && tb_s249[1] == 0); })) {
                v_ok = (v_t == 1LL);
            } else if (({ const char *tb_s250 = v_p; ((unsigned char)tb_s250[0] == 110 && tb_s250[1] == 0); })) {
                v_ok = f_check__is_number(v_t);
            } else if (({ const char *tb_s251 = v_p; ((unsigned char)tb_s251[0] == 76 && tb_s251[1] == 0); })) {
                v_ok = (f_check__is_list(v_c, v_t) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 387))->q_types, v_t, sizeof(tb_o_Type *), 387)), 387))->q_elem == 4LL));
            } else if (({ const char *tb_s252 = v_p; ((unsigned char)tb_s252[0] == 115 && tb_s252[1] == 0); })) {
                v_ok = (f_check__is_list(v_c, v_t) && (((((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 1LL) || (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 2LL)) || (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 4LL)));
            } else if (({ const char *tb_s253 = v_p; ((unsigned char)tb_s253[0] == 120 && tb_s253[1] == 0); })) {
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
    if (({ const char *tb_s254 = v_name; ((unsigned char)tb_s254[0] == 112 && (unsigned char)tb_s254[1] == 114 && (unsigned char)tb_s254[2] == 105 && (unsigned char)tb_s254[3] == 110 && (unsigned char)tb_s254[4] == 116 && tb_s254[5] == 0); })) {
        {
            long long tb_end255 = ((((tb_o_Node *)tb_nn(v_n, 398))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end255; v_i++) {
                long long v_t = f_check__builtin_arg(v_c, v_n, v_i);
            }
        }
        return 0LL;
    }
    if (({ const char *tb_s256 = v_name; ((unsigned char)tb_s256[0] == 108 && (unsigned char)tb_s256[1] == 101 && (unsigned char)tb_s256[2] == 110 && tb_s256[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "x", "len(список, текст или dict)");
        return 1LL;
    }
    if (({ const char *tb_s257 = v_name; ((unsigned char)tb_s257[0] == 114 && (unsigned char)tb_s257[1] == 111 && (unsigned char)tb_s257[2] == 117 && (unsigned char)tb_s257[3] == 110 && (unsigned char)tb_s257[4] == 100 && tb_s257[5] == 0); })) {
        f_check__builtin_args(v_c, v_n, "n", "round(число)");
        return 1LL;
    }
    if (({ const char *tb_s258 = v_name; ((unsigned char)tb_s258[0] == 99 && (unsigned char)tb_s258[1] == 111 && (unsigned char)tb_s258[2] == 100 && (unsigned char)tb_s258[3] == 101 && tb_s258[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "code(\"A\")  →  65");
        return 1LL;
    }
    if (({ const char *tb_s259 = v_name; ((unsigned char)tb_s259[0] == 99 && (unsigned char)tb_s259[1] == 104 && (unsigned char)tb_s259[2] == 97 && (unsigned char)tb_s259[3] == 114 && tb_s259[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "i", "char(66)  →  \"B\"");
        return 4LL;
    }
    if (({ const char *tb_s260 = v_name; ((unsigned char)tb_s260[0] == 99 && (unsigned char)tb_s260[1] == 117 && (unsigned char)tb_s260[2] == 116 && tb_s260[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "tii", "cut(текст, с какого номера, по какой номер)");
        return 4LL;
    }
    if (({ const char *tb_s261 = v_name; ((unsigned char)tb_s261[0] == 116 && (unsigned char)tb_s261[1] == 111 && (unsigned char)tb_s261[2] == 95 && (unsigned char)tb_s261[3] == 105 && (unsigned char)tb_s261[4] == 110 && (unsigned char)tb_s261[5] == 116 && tb_s261[6] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "to_int(\"42\")");
        return 1LL;
    }
    if (({ const char *tb_s262 = v_name; ((unsigned char)tb_s262[0] == 116 && (unsigned char)tb_s262[1] == 111 && (unsigned char)tb_s262[2] == 95 && (unsigned char)tb_s262[3] == 100 && (unsigned char)tb_s262[4] == 101 && (unsigned char)tb_s262[5] == 99 && tb_s262[6] == 0); })) {
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
    if (({ const char *tb_s263 = v_name; ((unsigned char)tb_s263[0] == 105 && (unsigned char)tb_s263[1] == 110 && (unsigned char)tb_s263[2] == 112 && (unsigned char)tb_s263[3] == 117 && (unsigned char)tb_s263[4] == 116 && tb_s263[5] == 0); })) {
        f_check__builtin_args(v_c, v_n, "", "input()");
        return 4LL;
    }
    if (({ const char *tb_s264 = v_name; ((unsigned char)tb_s264[0] == 97 && (unsigned char)tb_s264[1] == 114 && (unsigned char)tb_s264[2] == 103 && (unsigned char)tb_s264[3] == 115 && tb_s264[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "", "args()");
        return f_check__list_of(v_c, 4LL);
    }
    if (({ const char *tb_s265 = v_name; ((unsigned char)tb_s265[0] == 115 && (unsigned char)tb_s265[1] == 112 && (unsigned char)tb_s265[2] == 108 && (unsigned char)tb_s265[3] == 105 && (unsigned char)tb_s265[4] == 116 && tb_s265[5] == 0); })) {
        f_check__builtin_args(v_c, v_n, "tt", "split(текст, разделитель)");
        return f_check__list_of(v_c, 4LL);
    }
    if (({ const char *tb_s266 = v_name; ((unsigned char)tb_s266[0] == 106 && (unsigned char)tb_s266[1] == 111 && (unsigned char)tb_s266[2] == 105 && (unsigned char)tb_s266[3] == 110 && tb_s266[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "Lt", "join(список text, разделитель)");
        return 4LL;
    }
    if (({ const char *tb_s267 = v_name; ((unsigned char)tb_s267[0] == 102 && (unsigned char)tb_s267[1] == 105 && (unsigned char)tb_s267[2] == 110 && (unsigned char)tb_s267[3] == 100 && tb_s267[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "tt", "find(текст, что ищем)  →  номер или -1");
        return 1LL;
    }
    if (({ const char *tb_s268 = v_name; ((unsigned char)tb_s268[0] == 114 && (unsigned char)tb_s268[1] == 101 && (unsigned char)tb_s268[2] == 112 && (unsigned char)tb_s268[3] == 108 && (unsigned char)tb_s268[4] == 97 && (unsigned char)tb_s268[5] == 99 && (unsigned char)tb_s268[6] == 101 && tb_s268[7] == 0); })) {
        f_check__builtin_args(v_c, v_n, "ttt", "replace(текст, что, на что)");
        return 4LL;
    }
    if (((({ const char *tb_s269 = v_name; ((unsigned char)tb_s269[0] == 117 && (unsigned char)tb_s269[1] == 112 && (unsigned char)tb_s269[2] == 112 && (unsigned char)tb_s269[3] == 101 && (unsigned char)tb_s269[4] == 114 && tb_s269[5] == 0); }) || ({ const char *tb_s270 = v_name; ((unsigned char)tb_s270[0] == 108 && (unsigned char)tb_s270[1] == 111 && (unsigned char)tb_s270[2] == 119 && (unsigned char)tb_s270[3] == 101 && (unsigned char)tb_s270[4] == 114 && tb_s270[5] == 0); })) || ({ const char *tb_s271 = v_name; ((unsigned char)tb_s271[0] == 116 && (unsigned char)tb_s271[1] == 114 && (unsigned char)tb_s271[2] == 105 && (unsigned char)tb_s271[3] == 109 && tb_s271[4] == 0); }))) {
        f_check__builtin_args(v_c, v_n, "t", "upper(текст)");
        return 4LL;
    }
    if (((strcmp(v_name, "starts_with") == 0) || (strcmp(v_name, "ends_with") == 0))) {
        f_check__builtin_args(v_c, v_n, "tt", "starts_with(текст, начало)");
        return 3LL;
    }
    if (({ const char *tb_s272 = v_name; ((unsigned char)tb_s272[0] == 115 && (unsigned char)tb_s272[1] == 111 && (unsigned char)tb_s272[2] == 114 && (unsigned char)tb_s272[3] == 116 && tb_s272[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "s", "sort(список int, dec или text)");
        return 0LL;
    }
    if (({ const char *tb_s273 = v_name; ((unsigned char)tb_s273[0] == 114 && (unsigned char)tb_s273[1] == 117 && (unsigned char)tb_s273[2] == 110 && tb_s273[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "run(\"команда терминала\")  →  код выхода int");
        return 1LL;
    }
    if (({ const char *tb_s274 = v_name; ((unsigned char)tb_s274[0] == 101 && (unsigned char)tb_s274[1] == 120 && (unsigned char)tb_s274[2] == 105 && (unsigned char)tb_s274[3] == 116 && tb_s274[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "i", "exit(код)");
        return 0LL;
    }
    if ((strcmp(v_name, "file_exists") == 0)) {
        f_check__builtin_args(v_c, v_n, "t", "file_exists(\"путь\")");
        return 3LL;
    }
    if (({ const char *tb_s275 = v_name; ((unsigned char)tb_s275[0] == 97 && (unsigned char)tb_s275[1] == 98 && (unsigned char)tb_s275[2] == 115 && tb_s275[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "n", "abs(число)");
        return ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 466))->q_kids, 0LL, sizeof(tb_o_Node *), 466)), 466))->q_ty;
    }
    if (({ const char *tb_s276 = v_name; ((unsigned char)tb_s276[0] == 115 && (unsigned char)tb_s276[1] == 113 && (unsigned char)tb_s276[2] == 114 && (unsigned char)tb_s276[3] == 116 && tb_s276[4] == 0); })) {
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
        long long tb_end277 = v_np;
        for (long long v_j = 1LL; v_j <= tb_end277; v_j++) {
            { tb_o_Node * tb_v278 = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *))); *(tb_o_Node * *)tb_grow(v_ordered) = tb_v278; }
            { _Bool tb_v279 = 0; tb_badd(v_taken, tb_v279); }
        }
    }
    _Bool v_named = 0;
    {
        long long tb_end280 = ((((tb_o_Node *)tb_nn(v_call, 488))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end280; v_i++) {
            tb_o_Node * v_arg = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_call, 489))->q_kids, v_i, sizeof(tb_o_Node *), 489));
            long long v_slot = v_i;
            if (({ const char *tb_s281 = ((tb_o_Node *)tb_nn(v_arg, 491))->q_name; !(tb_s281[0] == 0); })) {
                v_named = 1;
                v_slot = (-1LL);
                {
                    long long tb_end282 = (v_np - 1LL);
                    for (long long v_j = 0LL; v_j <= tb_end282; v_j++) {
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
            { tb_o_Node * tb_v283 = v_arg; tb_o_Node * *tb_p283 = &((*(tb_o_Node * *)tb_at(v_ordered, v_slot, sizeof(tb_o_Node *), 503))); *tb_p283 = tb_v283; }
            { _Bool tb_v284 = 1; tb_bset(v_taken, v_slot, tb_v284, 504); }
        }
    }
    { tb_list * tb_v285 = v_ordered; tb_list * *tb_p285 = &(((tb_o_Node *)tb_nn(v_call, 506))->q_kids); *tb_p285 = tb_v285; }
}

static void f_check__need_place_for(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_why, _Bool v_for_link) {
    if ((({ const char *tb_s286 = ((tb_o_Node *)tb_nn(v_n, 510))->q_kind; ((unsigned char)tb_s286[0] == 105 && (unsigned char)tb_s286[1] == 110 && (unsigned char)tb_s286[2] == 100 && (unsigned char)tb_s286[3] == 101 && (unsigned char)tb_s286[4] == 120 && tb_s286[5] == 0); }) && f_check__is_dict(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 510))->q_kids, 0LL, sizeof(tb_o_Node *), 510)), 510))->q_ty))) {
        if (v_for_link) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 512))->q_line, "ярлык на значение в dict нельзя: dict может переложить значения", "");
        }
        return;
    }
    if (({ const char *tb_s287 = ((tb_o_Node *)tb_nn(v_n, 514))->q_kind; ((unsigned char)tb_s287[0] == 110 && (unsigned char)tb_s287[1] == 97 && (unsigned char)tb_s287[2] == 109 && (unsigned char)tb_s287[3] == 101 && tb_s287[4] == 0); })) {
        long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 515))->q_name);
        if (((v_v >= 0LL) && ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 516))->q_vars, v_v, sizeof(tb_o_Var *), 516)), 516))->q_is_const)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 517))->q_line, "это const или переменная цикла, её нельзя менять", ((tb_o_Node *)tb_nn(v_n, 517))->q_name);
        }
        return;
    }
    if (({ const char *tb_s288 = ((tb_o_Node *)tb_nn(v_n, 519))->q_kind; ((unsigned char)tb_s288[0] == 102 && (unsigned char)tb_s288[1] == 105 && (unsigned char)tb_s288[2] == 101 && (unsigned char)tb_s288[3] == 108 && (unsigned char)tb_s288[4] == 100 && tb_s288[5] == 0); })) {
        return;
    }
    if ((((({ const char *tb_s289 = ((tb_o_Node *)tb_nn(v_n, 521))->q_kind; ((unsigned char)tb_s289[0] == 105 && (unsigned char)tb_s289[1] == 110 && (unsigned char)tb_s289[2] == 100 && (unsigned char)tb_s289[3] == 101 && (unsigned char)tb_s289[4] == 120 && tb_s289[5] == 0); }) && f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 521))->q_kids, 0LL, sizeof(tb_o_Node *), 521)), 521))->q_ty)) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 521))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 521))->q_kids, 0LL, sizeof(tb_o_Node *), 521)), 521))->q_ty, sizeof(tb_o_Type *), 521)), 521))->q_elem == 3LL)) && v_for_link)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 522))->q_line, "ярлык на элемент list bool нельзя: там каждый элемент — один бит, у бита нет своего адреса", "");
    }
    if (((({ const char *tb_s290 = ((tb_o_Node *)tb_nn(v_n, 523))->q_kind; ((unsigned char)tb_s290[0] == 105 && (unsigned char)tb_s290[1] == 110 && (unsigned char)tb_s290[2] == 100 && (unsigned char)tb_s290[3] == 101 && (unsigned char)tb_s290[4] == 120 && tb_s290[5] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 523))->q_kids, 0LL, sizeof(tb_o_Node *), 523)), 523))->q_ty)) && v_for_link)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 524))->q_line, "ярлык на элемент списка колонок нельзя: возьми ярлык на качество, например pts[i].x", "");
    }
    if ((({ const char *tb_s291 = ((tb_o_Node *)tb_nn(v_n, 525))->q_kind; ((unsigned char)tb_s291[0] == 105 && (unsigned char)tb_s291[1] == 110 && (unsigned char)tb_s291[2] == 100 && (unsigned char)tb_s291[3] == 101 && (unsigned char)tb_s291[4] == 120 && tb_s291[5] == 0); }) && f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 525))->q_kids, 0LL, sizeof(tb_o_Node *), 525)), 525))->q_ty))) {
        return;
    }
    if (({ const char *tb_s292 = ((tb_o_Node *)tb_nn(v_n, 527))->q_kind; ((unsigned char)tb_s292[0] == 105 && (unsigned char)tb_s292[1] == 110 && (unsigned char)tb_s292[2] == 100 && (unsigned char)tb_s292[3] == 101 && (unsigned char)tb_s292[4] == 120 && tb_s292[5] == 0); })) {
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
        long long tb_end293 = ((((tb_o_Node *)tb_nn(v_n, 536))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end293; v_i++) {
            if (({ const char *tb_s294 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 537))->q_kids, v_i, sizeof(tb_o_Node *), 537)), 537))->q_kind; ((unsigned char)tb_s294[0] == 109 && (unsigned char)tb_s294[1] == 105 && (unsigned char)tb_s294[2] == 115 && (unsigned char)tb_s294[3] == 115 && (unsigned char)tb_s294[4] == 105 && (unsigned char)tb_s294[5] == 110 && (unsigned char)tb_s294[6] == 103 && tb_s294[7] == 0); })) {
                continue;
            }
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 539))->q_kids, v_i, sizeof(tb_o_Node *), 539)), 539))->q_kids, 0LL, sizeof(tb_o_Node *), 539));
            long long v_t = f_check__check_value(v_c, v_value);
            if ((!f_check__fits(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 541))->q_kids, v_i, sizeof(tb_o_Node *), 541)), 541))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 542))->q_line, "качество другого типа", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 542))->q_kids, v_i, sizeof(tb_o_Node *), 542)), 542))->q_name);
            }
            { long long tb_v295 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 543))->q_kids, v_i, sizeof(tb_o_Node *), 543)), 543))->q_ty; long long *tb_p295 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 543))->q_kids, v_i, sizeof(tb_o_Node *), 543)), 543))->q_ty); *tb_p295 = tb_v295; }
        }
    }
    { tb_o_Node * tb_v296 = v_o; tb_o_Node * *tb_p296 = &(((tb_o_Node *)tb_nn(v_n, 544))->q_ref); *tb_p296 = tb_v296; }
    { _Bool tb_v297 = 1; _Bool *tb_p297 = &(((tb_o_Node *)tb_nn(v_n, 545))->q_has_ref); *tb_p297 = tb_v297; }
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
    if (({ const char *tb_s298 = ((tb_o_Node *)tb_nn(v_n, 566))->q_name; ((unsigned char)tb_s298[0] == 109 && (unsigned char)tb_s298[1] == 97 && (unsigned char)tb_s298[2] == 105 && (unsigned char)tb_s298[3] == 110 && tb_s298[4] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 567))->q_line, "main запускается сама, её нельзя вызывать", "");
    }
    if ((((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 568))->q_node, 568))->q_can_fail && (!((tb_o_Node *)tb_nn(v_n, 568))->q_creates))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 569))->q_line, "функция может закончиться ошибкой: напиши  try f(),  f() or запасное  или  f() else err", ((tb_o_Node *)tb_nn(v_n, 569))->q_name);
    }
    { tb_o_Node * tb_v299 = ((tb_o_Func *)tb_nn(v_f, 570))->q_node; tb_o_Node * *tb_p299 = &(((tb_o_Node *)tb_nn(v_n, 570))->q_ref); *tb_p299 = tb_v299; }
    { _Bool tb_v300 = 1; _Bool *tb_p300 = &(((tb_o_Node *)tb_nn(v_n, 571))->q_has_ref); *tb_p300 = tb_v300; }
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 572))->q_node, 572))->q_kids, 0LL, sizeof(tb_o_Node *), 572));
    f_check__order_args(v_c, v_n, v_params, 0);
    {
        long long tb_end301 = ((((tb_o_Node *)tb_nn(v_n, 574))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end301; v_i++) {
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
                    { long long tb_v302 = v_t; long long *tb_p302 = &(((tb_o_Node *)tb_nn(v_p, 585))->q_ty); *tb_p302 = tb_v302; }
                } else if ((((tb_o_Node *)tb_nn(v_p, 586))->q_ty != v_t)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 587))->q_line, "этот ярлык уже был другого типа", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_p, 587))->q_ty));
                }
            } else if ((!f_check__fits(((tb_o_Node *)tb_nn(v_p, 588))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 589))->q_line, "параметр ждёт другой тип", ((tb_o_Node *)tb_nn(v_p, 589))->q_name);
            }
            { long long tb_v303 = ((tb_o_Node *)tb_nn(v_p, 590))->q_ty; long long *tb_p303 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 590))->q_kids, v_i, sizeof(tb_o_Node *), 590)), 590))->q_ty); *tb_p303 = tb_v303; }
        }
    }
    { long long tb_v304 = ((tb_o_Comp *)tb_nn(v_c, 591))->q_current; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 591))->q_call_from) = tb_v304; }
    { long long tb_v305 = v_fi; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 592))->q_call_to) = tb_v305; }
    { _Bool tb_v306 = (((tb_o_Comp *)tb_nn(v_c, 593))->q_loop_depth > 0LL); tb_badd(((tb_o_Comp *)tb_nn(v_c, 593))->q_call_loop, tb_v306); }
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
        { tb_list * tb_v307 = v_fresh_loops; tb_list * *tb_p307 = &(((tb_o_Comp *)tb_nn(v_c, 609))->q_loops); *tb_p307 = tb_v307; }
        { long long tb_v308 = (-1LL); long long *tb_p308 = &(((tb_o_Comp *)tb_nn(v_c, 610))->q_par_base); *tb_p308 = tb_v308; }
        { long long tb_v309 = (-1LL); long long *tb_p309 = &(((tb_o_Comp *)tb_nn(v_c, 611))->q_temp_base); *tb_p309 = tb_v309; }
        { long long tb_v310 = 0LL; long long *tb_p310 = &(((tb_o_Comp *)tb_nn(v_c, 612))->q_piece_depth); *tb_p310 = tb_v310; }
        { long long tb_v311 = (((tb_o_Comp *)tb_nn(v_c, 613))->q_vars)->len; long long *tb_p311 = &(((tb_o_Comp *)tb_nn(v_c, 613))->q_var_base); *tb_p311 = tb_v311; }
        f_check__check_func(v_c, v_fi);
        { long long tb_v312 = v_saved_pieces; long long *tb_p312 = &(((tb_o_Comp *)tb_nn(v_c, 615))->q_piece_depth); *tb_p312 = tb_v312; }
        { tb_list * tb_v313 = v_saved_loop_stack; tb_list * *tb_p313 = &(((tb_o_Comp *)tb_nn(v_c, 616))->q_loops); *tb_p313 = tb_v313; }
        { long long tb_v314 = v_saved_par; long long *tb_p314 = &(((tb_o_Comp *)tb_nn(v_c, 617))->q_par_base); *tb_p314 = tb_v314; }
        { long long tb_v315 = v_saved_temp; long long *tb_p315 = &(((tb_o_Comp *)tb_nn(v_c, 618))->q_temp_base); *tb_p315 = tb_v315; }
        { long long tb_v316 = v_saved_temp_loops; long long *tb_p316 = &(((tb_o_Comp *)tb_nn(v_c, 619))->q_temp_loops); *tb_p316 = tb_v316; }
        { long long tb_v317 = v_saved_base; long long *tb_p317 = &(((tb_o_Comp *)tb_nn(v_c, 620))->q_var_base); *tb_p317 = tb_v317; }
        { const char * tb_v318 = v_saved_mod; const char * *tb_p318 = &(((tb_o_Comp *)tb_nn(v_c, 621))->q_cur_mod); *tb_p318 = tb_v318; }
        { const char * tb_v319 = v_saved_file; const char * *tb_p319 = &(((tb_o_Comp *)tb_nn(v_c, 622))->q_file_name); *tb_p319 = tb_v319; }
        { long long tb_v320 = v_saved_current; long long *tb_p320 = &(((tb_o_Comp *)tb_nn(v_c, 623))->q_current); *tb_p320 = tb_v320; }
        { long long tb_v321 = v_saved_loops; long long *tb_p321 = &(((tb_o_Comp *)tb_nn(v_c, 624))->q_loop_depth); *tb_p321 = tb_v321; }
        f_check__cut_vars(v_c, v_saved_vars);
        { long long tb_v322 = v_saved_depth; long long *tb_p322 = &(((tb_o_Comp *)tb_nn(v_c, 626))->q_depth); *tb_p322 = tb_v322; }
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
    { const char * tb_v323 = "call"; const char * *tb_p323 = &(((tb_o_Node *)tb_nn(v_n, 642))->q_kind); *tb_p323 = tb_v323; }
    tb_list * v_rest = tb_list_new(sizeof(tb_o_Node *));
    {
        long long tb_end324 = ((((tb_o_Node *)tb_nn(v_n, 644))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end324; v_i++) {
            { tb_o_Node * tb_v325 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 645))->q_kids, v_i, sizeof(tb_o_Node *), 645)); *(tb_o_Node * *)tb_grow(v_rest) = tb_v325; }
        }
    }
    { tb_list * tb_v326 = v_rest; tb_list * *tb_p326 = &(((tb_o_Node *)tb_nn(v_n, 646))->q_kids); *tb_p326 = tb_v326; }
    return f_check__check_user_call(v_c, v_n, v_f);
    return v_r;
}

static long long f_check__check_dict_method(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t) {
    long long v_r = 0LL;
    long long v_args = ((((tb_o_Node *)tb_nn(v_n, 650))->q_kids)->len - 1LL);
    tb_o_Type * v_d = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 651))->q_types, v_t, sizeof(tb_o_Type *), 651));
    if ((({ const char *tb_s327 = ((tb_o_Node *)tb_nn(v_n, 652))->q_name; ((unsigned char)tb_s327[0] == 104 && (unsigned char)tb_s327[1] == 97 && (unsigned char)tb_s327[2] == 115 && tb_s327[3] == 0); }) || ({ const char *tb_s328 = ((tb_o_Node *)tb_nn(v_n, 652))->q_name; ((unsigned char)tb_s328[0] == 114 && (unsigned char)tb_s328[1] == 101 && (unsigned char)tb_s328[2] == 109 && (unsigned char)tb_s328[3] == 111 && (unsigned char)tb_s328[4] == 118 && (unsigned char)tb_s328[5] == 101 && tb_s328[6] == 0); }))) {
        if ((v_args != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 654))->q_line, "нужен ровно один ключ", ((tb_o_Node *)tb_nn(v_n, 654))->q_name);
        }
        long long v_k = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 655))->q_kids, 1LL, sizeof(tb_o_Node *), 655)), 655))->q_kids, 0LL, sizeof(tb_o_Node *), 655)));
        if ((v_k != ((tb_o_Type *)tb_nn(v_d, 656))->q_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 657))->q_line, "ключ у этого dict другого типа", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_d, 657))->q_key));
        }
        { long long tb_v329 = ((tb_o_Type *)tb_nn(v_d, 658))->q_key; long long *tb_p329 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 658))->q_kids, 1LL, sizeof(tb_o_Node *), 658)), 658))->q_ty); *tb_p329 = tb_v329; }
        if (({ const char *tb_s330 = ((tb_o_Node *)tb_nn(v_n, 659))->q_name; ((unsigned char)tb_s330[0] == 104 && (unsigned char)tb_s330[1] == 97 && (unsigned char)tb_s330[2] == 115 && tb_s330[3] == 0); })) {
            return 3LL;
        }
        return 0LL;
    }
    if ((({ const char *tb_s331 = ((tb_o_Node *)tb_nn(v_n, 662))->q_name; ((unsigned char)tb_s331[0] == 107 && (unsigned char)tb_s331[1] == 101 && (unsigned char)tb_s331[2] == 121 && (unsigned char)tb_s331[3] == 115 && tb_s331[4] == 0); }) || ({ const char *tb_s332 = ((tb_o_Node *)tb_nn(v_n, 662))->q_name; ((unsigned char)tb_s332[0] == 118 && (unsigned char)tb_s332[1] == 97 && (unsigned char)tb_s332[2] == 108 && (unsigned char)tb_s332[3] == 117 && (unsigned char)tb_s332[4] == 101 && (unsigned char)tb_s332[5] == 115 && tb_s332[6] == 0); }))) {
        if ((v_args != 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 664))->q_line, "пишется без значений", ((tb_o_Node *)tb_nn(v_n, 664))->q_name);
        }
        if (({ const char *tb_s333 = ((tb_o_Node *)tb_nn(v_n, 665))->q_name; ((unsigned char)tb_s333[0] == 107 && (unsigned char)tb_s333[1] == 101 && (unsigned char)tb_s333[2] == 121 && (unsigned char)tb_s333[3] == 115 && tb_s333[4] == 0); })) {
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
    if (((({ const char *tb_s334 = ((tb_o_Node *)tb_nn(v_base, 673))->q_kind; ((unsigned char)tb_s334[0] == 110 && (unsigned char)tb_s334[1] == 97 && (unsigned char)tb_s334[2] == 109 && (unsigned char)tb_s334[3] == 101 && tb_s334[4] == 0); }) && (f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_base, 673))->q_name) < 0LL)) && f_check__is_module(v_c, ((tb_o_Node *)tb_nn(v_base, 673))->q_name))) {
        return f_check__check_module_call(v_c, v_n);
    }
    long long v_t = f_check__check_value(v_c, v_base);
    long long v_args = ((((tb_o_Node *)tb_nn(v_n, 676))->q_kids)->len - 1LL);
    {
        long long tb_end335 = ((((tb_o_Node *)tb_nn(v_n, 677))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end335; v_i++) {
            if (({ const char *tb_s336 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 678))->q_kids, v_i, sizeof(tb_o_Node *), 678)), 678))->q_name; !(tb_s336[0] == 0); })) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 679))->q_line, "у методов аргументы пишутся без имён", "");
            }
        }
    }
    if (((({ const char *tb_s337 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s337[0] == 97 && (unsigned char)tb_s337[1] == 100 && (unsigned char)tb_s337[2] == 100 && tb_s337[3] == 0); }) || ({ const char *tb_s338 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s338[0] == 112 && (unsigned char)tb_s338[1] == 111 && (unsigned char)tb_s338[2] == 112 && tb_s338[3] == 0); })) || ({ const char *tb_s339 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s339[0] == 114 && (unsigned char)tb_s339[1] == 101 && (unsigned char)tb_s339[2] == 109 && (unsigned char)tb_s339[3] == 111 && (unsigned char)tb_s339[4] == 118 && (unsigned char)tb_s339[5] == 101 && tb_s339[6] == 0); }))) {
        f_check__no_outside_change(v_c, v_base);
    }
    if (f_check__is_dict(v_c, v_t)) {
        return f_check__check_dict_method(v_c, v_n, v_t);
    }
    if ((!f_check__is_list(v_c, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 685))->q_line, "у этого значения нет методов: они есть у списков (.add .pop) и dict (.has .remove .keys .values)", ((tb_o_Node *)tb_nn(v_n, 685))->q_name);
    }
    long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 686))->q_types, v_t, sizeof(tb_o_Type *), 686)), 686))->q_elem;
    if (({ const char *tb_s340 = ((tb_o_Node *)tb_nn(v_n, 687))->q_name; ((unsigned char)tb_s340[0] == 97 && (unsigned char)tb_s340[1] == 100 && (unsigned char)tb_s340[2] == 100 && tb_s340[3] == 0); })) {
        if ((v_args != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 689))->q_line, ".add кладёт ровно одно значение", "");
        }
        long long v_v = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 690))->q_kids, 1LL, sizeof(tb_o_Node *), 690)), 690))->q_kids, 0LL, sizeof(tb_o_Node *), 690)));
        if ((!f_check__fits(v_elem, v_v))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 692))->q_line, "в этот список кладут другой тип", f_check__type_name(v_c, v_elem));
        }
        { long long tb_v341 = v_elem; long long *tb_p341 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 693))->q_kids, 1LL, sizeof(tb_o_Node *), 693)), 693))->q_ty); *tb_p341 = tb_v341; }
        return 0LL;
    }
    if (({ const char *tb_s342 = ((tb_o_Node *)tb_nn(v_n, 695))->q_name; ((unsigned char)tb_s342[0] == 112 && (unsigned char)tb_s342[1] == 111 && (unsigned char)tb_s342[2] == 112 && tb_s342[3] == 0); })) {
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
        { long long tb_v343 = 2LL; long long *tb_p343 = &(v_i); *tb_p343 += tb_v343; }
    }
    return f_check__dict_of(v_c, v_key, v_value);
    return v_r;
}

static _Bool f_check__can_fail_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if (({ const char *tb_s344 = ((tb_o_Node *)tb_nn(v_n, 726))->q_kind; ((unsigned char)tb_s344[0] == 99 && (unsigned char)tb_s344[1] == 97 && (unsigned char)tb_s344[2] == 108 && (unsigned char)tb_s344[3] == 108 && tb_s344[4] == 0); })) {
        if (f_check__is_builtin(((tb_o_Node *)tb_nn(v_n, 727))->q_name)) {
            return f_check__builtin_can_fail(((tb_o_Node *)tb_nn(v_n, 728))->q_name);
        }
        if (((f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 729))->q_name) >= 0LL) || (f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_n, 729))->q_name) >= 0LL))) {
            return 0;
        }
        long long v_f = f_check__find_func(v_c, ((tb_o_Node *)tb_nn(v_n, 731))->q_name);
        return ((v_f >= 0LL) && ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 732))->q_funcs, v_f, sizeof(tb_o_Func *), 732)), 732))->q_node, 732))->q_can_fail);
    }
    if ((((({ const char *tb_s345 = ((tb_o_Node *)tb_nn(v_n, 733))->q_kind; ((unsigned char)tb_s345[0] == 109 && (unsigned char)tb_s345[1] == 101 && (unsigned char)tb_s345[2] == 116 && (unsigned char)tb_s345[3] == 104 && (unsigned char)tb_s345[4] == 111 && (unsigned char)tb_s345[5] == 100 && tb_s345[6] == 0); }) && ({ const char *tb_s346 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 733))->q_kids, 0LL, sizeof(tb_o_Node *), 733)), 733))->q_kind; ((unsigned char)tb_s346[0] == 110 && (unsigned char)tb_s346[1] == 97 && (unsigned char)tb_s346[2] == 109 && (unsigned char)tb_s346[3] == 101 && tb_s346[4] == 0); })) && (f_check__find_var(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 733))->q_kids, 0LL, sizeof(tb_o_Node *), 733)), 733))->q_name) < 0LL)) && f_check__is_module(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 733))->q_kids, 0LL, sizeof(tb_o_Node *), 733)), 733))->q_name))) {
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
    { _Bool tb_v347 = 1; _Bool *tb_p347 = &(((tb_o_Node *)tb_nn(v_n, 742))->q_creates); *tb_p347 = tb_v347; }
    return f_check__check_expr(v_c, v_n);
    return v_r;
}

static long long f_check__check_expr(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    long long v_t = (-1LL);
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 747))->q_kind;
    if (({ const char *tb_s348 = v_k; ((unsigned char)tb_s348[0] == 110 && (unsigned char)tb_s348[1] == 117 && (unsigned char)tb_s348[2] == 109 && (unsigned char)tb_s348[3] == 98 && (unsigned char)tb_s348[4] == 101 && (unsigned char)tb_s348[5] == 114 && tb_s348[6] == 0); })) {
        v_t = 1LL;
    } else if (({ const char *tb_s349 = v_k; ((unsigned char)tb_s349[0] == 100 && (unsigned char)tb_s349[1] == 101 && (unsigned char)tb_s349[2] == 99 && (unsigned char)tb_s349[3] == 105 && (unsigned char)tb_s349[4] == 109 && (unsigned char)tb_s349[5] == 97 && (unsigned char)tb_s349[6] == 108 && tb_s349[7] == 0); })) {
        v_t = 2LL;
    } else if (({ const char *tb_s350 = v_k; ((unsigned char)tb_s350[0] == 115 && (unsigned char)tb_s350[1] == 116 && (unsigned char)tb_s350[2] == 114 && (unsigned char)tb_s350[3] == 105 && (unsigned char)tb_s350[4] == 110 && (unsigned char)tb_s350[5] == 103 && tb_s350[6] == 0); })) {
        v_t = 4LL;
    } else if (({ const char *tb_s351 = v_k; ((unsigned char)tb_s351[0] == 98 && (unsigned char)tb_s351[1] == 111 && (unsigned char)tb_s351[2] == 111 && (unsigned char)tb_s351[3] == 108 && tb_s351[4] == 0); })) {
        v_t = 3LL;
    } else if (({ const char *tb_s352 = v_k; ((unsigned char)tb_s352[0] == 110 && (unsigned char)tb_s352[1] == 97 && (unsigned char)tb_s352[2] == 109 && (unsigned char)tb_s352[3] == 101 && tb_s352[4] == 0); })) {
        long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 757))->q_name);
        if ((v_v < 0LL)) {
            if ((f_check__find_func(v_c, ((tb_o_Node *)tb_nn(v_n, 759))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 760))->q_line, "у функции нужны скобки", ((tb_o_Node *)tb_nn(v_n, 760))->q_name);
            }
            if ((f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_n, 761))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 762))->q_line, "чтобы создать obj, нужны скобки", tb_cat(((tb_o_Node *)tb_nn(v_n, 762))->q_name, "(...)"));
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
        { _Bool tb_v353 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 771))->q_vars, v_v, sizeof(tb_o_Var *), 771)), 771))->q_is_link; _Bool *tb_p353 = &(((tb_o_Node *)tb_nn(v_n, 771))->q_is_link); *tb_p353 = tb_v353; }
        v_t = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 772))->q_vars, v_v, sizeof(tb_o_Var *), 772)), 772))->q_type;
    } else if (({ const char *tb_s354 = v_k; ((unsigned char)tb_s354[0] == 108 && (unsigned char)tb_s354[1] == 105 && (unsigned char)tb_s354[2] == 115 && (unsigned char)tb_s354[3] == 116 && tb_s354[4] == 0); })) {
        v_t = f_check__check_list(v_c, v_n);
    } else if (({ const char *tb_s355 = v_k; ((unsigned char)tb_s355[0] == 100 && (unsigned char)tb_s355[1] == 105 && (unsigned char)tb_s355[2] == 99 && (unsigned char)tb_s355[3] == 116 && tb_s355[4] == 0); })) {
        v_t = f_check__check_dict(v_c, v_n);
    } else if (({ const char *tb_s356 = v_k; ((unsigned char)tb_s356[0] == 116 && (unsigned char)tb_s356[1] == 114 && (unsigned char)tb_s356[2] == 121 && tb_s356[3] == 0); })) {
        if ((((tb_o_Comp *)tb_nn(v_c, 778))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 779))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " try нельзя: ядро не может остановить всю функцию, пиши  or  или  else err"), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 780))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 781))->q_line, "внутри temp try нельзя: пиши  or  или  else err", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 782))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 783))->q_line, "внутри each ... by N try нельзя: пиши  or  или  else err", "");
        }
        v_t = f_check__check_handled(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 784))->q_kids, 0LL, sizeof(tb_o_Node *), 784)), "try");
    } else if (({ const char *tb_s357 = v_k; ((unsigned char)tb_s357[0] == 101 && (unsigned char)tb_s357[1] == 108 && (unsigned char)tb_s357[2] == 115 && (unsigned char)tb_s357[3] == 101 && (unsigned char)tb_s357[4] == 95 && (unsigned char)tb_s357[5] == 101 && (unsigned char)tb_s357[6] == 114 && (unsigned char)tb_s357[7] == 114 && tb_s357[8] == 0); })) {
        v_t = f_check__check_handled(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 786))->q_kids, 0LL, sizeof(tb_o_Node *), 786)), "else");
        f_check__open_scope(v_c);
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 788))->q_name, 4LL, 1, 0, ((tb_o_Node *)tb_nn(v_n, 788))->q_line);
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 789))->q_kids, 1LL, sizeof(tb_o_Node *), 789)));
        f_check__close_scope(v_c);
    } else if (({ const char *tb_s358 = v_k; ((unsigned char)tb_s358[0] == 99 && (unsigned char)tb_s358[1] == 97 && (unsigned char)tb_s358[2] == 108 && (unsigned char)tb_s358[3] == 108 && tb_s358[4] == 0); })) {
        v_t = f_check__check_call(v_c, v_n);
    } else if (({ const char *tb_s359 = v_k; ((unsigned char)tb_s359[0] == 109 && (unsigned char)tb_s359[1] == 101 && (unsigned char)tb_s359[2] == 116 && (unsigned char)tb_s359[3] == 104 && (unsigned char)tb_s359[4] == 111 && (unsigned char)tb_s359[5] == 100 && tb_s359[6] == 0); })) {
        v_t = f_check__check_method(v_c, v_n);
    } else if (({ const char *tb_s360 = v_k; ((unsigned char)tb_s360[0] == 98 && (unsigned char)tb_s360[1] == 105 && (unsigned char)tb_s360[2] == 110 && (unsigned char)tb_s360[3] == 111 && (unsigned char)tb_s360[4] == 112 && tb_s360[5] == 0); })) {
        v_t = f_check__check_binop(v_c, v_n);
    } else if (({ const char *tb_s361 = v_k; ((unsigned char)tb_s361[0] == 117 && (unsigned char)tb_s361[1] == 110 && (unsigned char)tb_s361[2] == 97 && (unsigned char)tb_s361[3] == 114 && (unsigned char)tb_s361[4] == 121 && tb_s361[5] == 0); })) {
        long long v_inner = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 798))->q_kids, 0LL, sizeof(tb_o_Node *), 798)));
        if (({ const char *tb_s362 = ((tb_o_Node *)tb_nn(v_n, 799))->q_value; ((unsigned char)tb_s362[0] == 110 && (unsigned char)tb_s362[1] == 111 && (unsigned char)tb_s362[2] == 116 && tb_s362[3] == 0); })) {
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
    } else if ((({ const char *tb_s363 = v_k; ((unsigned char)tb_s363[0] == 105 && (unsigned char)tb_s363[1] == 110 && (unsigned char)tb_s363[2] == 100 && (unsigned char)tb_s363[3] == 101 && (unsigned char)tb_s363[4] == 120 && tb_s363[5] == 0); }) && f_check__is_dict(v_c, f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 807))->q_kids, 0LL, sizeof(tb_o_Node *), 807)))))) {
        tb_o_Type * v_base = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 808))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 808))->q_kids, 0LL, sizeof(tb_o_Node *), 808)), 808))->q_ty, sizeof(tb_o_Type *), 808));
        if ((f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 809))->q_kids, 1LL, sizeof(tb_o_Node *), 809))) != ((tb_o_Type *)tb_nn(v_base, 809))->q_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 810))->q_line, "ключ у этого dict другого типа", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_base, 810))->q_key));
        }
        v_t = ((tb_o_Type *)tb_nn(v_base, 811))->q_elem;
    } else if (({ const char *tb_s364 = v_k; ((unsigned char)tb_s364[0] == 105 && (unsigned char)tb_s364[1] == 110 && (unsigned char)tb_s364[2] == 100 && (unsigned char)tb_s364[3] == 101 && (unsigned char)tb_s364[4] == 120 && tb_s364[5] == 0); })) {
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
    } else if (((({ const char *tb_s365 = v_k; ((unsigned char)tb_s365[0] == 102 && (unsigned char)tb_s365[1] == 105 && (unsigned char)tb_s365[2] == 101 && (unsigned char)tb_s365[3] == 108 && (unsigned char)tb_s365[4] == 100 && tb_s365[5] == 0); }) && ({ const char *tb_s366 = ((tb_o_Node *)tb_nn(v_n, 823))->q_name; ((unsigned char)tb_s366[0] == 119 && (unsigned char)tb_s366[1] == 97 && (unsigned char)tb_s366[2] == 105 && (unsigned char)tb_s366[3] == 116 && tb_s366[4] == 0); })) && ({ const char *tb_s367 = f_check__kind_of(v_c, f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 823))->q_kids, 0LL, sizeof(tb_o_Node *), 823)))); ((unsigned char)tb_s367[0] == 116 && (unsigned char)tb_s367[1] == 97 && (unsigned char)tb_s367[2] == 115 && (unsigned char)tb_s367[3] == 107 && tb_s367[4] == 0); }))) {
        if ((((tb_o_Comp *)tb_nn(v_c, 824))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 825))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " .wait нельзя: забери готовое до цикла или после"), "");
        }
        v_t = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 826))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 826))->q_kids, 0LL, sizeof(tb_o_Node *), 826)), 826))->q_ty, sizeof(tb_o_Type *), 826)), 826))->q_elem;
    } else if ((strcmp(v_k, "background") == 0)) {
        v_t = f_check__check_background(v_c, v_n);
    } else if (({ const char *tb_s368 = v_k; ((unsigned char)tb_s368[0] == 102 && (unsigned char)tb_s368[1] == 105 && (unsigned char)tb_s368[2] == 101 && (unsigned char)tb_s368[3] == 108 && (unsigned char)tb_s368[4] == 100 && tb_s368[5] == 0); })) {
        long long v_base = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 830))->q_kids, 0LL, sizeof(tb_o_Node *), 830)));
        if (({ const char *tb_s369 = f_check__kind_of(v_c, v_base); !((unsigned char)tb_s369[0] == 111 && (unsigned char)tb_s369[1] == 98 && (unsigned char)tb_s369[2] == 106 && tb_s369[3] == 0); })) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 832))->q_line, "точка с качеством бывает только у obj", ((tb_o_Node *)tb_nn(v_n, 832))->q_name);
        }
        tb_o_Node * v_o = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 833))->q_types, v_base, sizeof(tb_o_Type *), 833)), 833))->q_obj_node;
        long long v_field = f_check__find_field(v_o, ((tb_o_Node *)tb_nn(v_n, 834))->q_name);
        if ((v_field < 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 836))->q_line, "у этого obj нет такого качества", ((tb_o_Node *)tb_nn(v_n, 836))->q_name);
        }
        v_t = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 837))->q_kids, v_field, sizeof(tb_o_Node *), 837)), 837))->q_ty;
        { tb_o_Node * tb_v370 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 838))->q_kids, v_field, sizeof(tb_o_Node *), 838)); *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 838))->q_acc_field) = tb_v370; }
        { long long tb_v371 = ((tb_o_Comp *)tb_nn(v_c, 839))->q_current; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 839))->q_acc_func) = tb_v371; }
        { _Bool tb_v372 = (((tb_o_Comp *)tb_nn(v_c, 840))->q_loop_depth > 0LL); tb_badd(((tb_o_Comp *)tb_nn(v_c, 840))->q_acc_loop, tb_v372); }
    }
    { long long tb_v373 = v_t; long long *tb_p373 = &(((tb_o_Node *)tb_nn(v_n, 841))->q_ty); *tb_p373 = tb_v373; }
    if (({ const char *tb_s374 = v_k; ((unsigned char)tb_s374[0] == 105 && (unsigned char)tb_s374[1] == 110 && (unsigned char)tb_s374[2] == 100 && (unsigned char)tb_s374[3] == 101 && (unsigned char)tb_s374[4] == 120 && tb_s374[5] == 0); })) {
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
    if ((({ const char *tb_s375 = ((tb_o_Node *)tb_nn(v_a, 856))->q_kind; !((unsigned char)tb_s375[0] == 110 && (unsigned char)tb_s375[1] == 97 && (unsigned char)tb_s375[2] == 109 && (unsigned char)tb_s375[3] == 101 && tb_s375[4] == 0); }) || (!f_check__declared_before(v_c, ((tb_o_Node *)tb_nn(v_a, 856))->q_name, ((tb_o_Node *)tb_nn(v_loop, 856))->q_name)))) {
        return;
    }
    if ((!((f_check__is_list(v_c, ((tb_o_Node *)tb_nn(v_a, 858))->q_ty) && (!f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_a, 858))->q_ty))) || f_check__is_dict(v_c, ((tb_o_Node *)tb_nn(v_a, 858))->q_ty)))) {
        return;
    }
    tb_o_Node * v_i = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 860))->q_kids, 1LL, sizeof(tb_o_Node *), 860));
    tb_o_Node * v_plan = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    if (({ const char *tb_s376 = ((tb_o_Node *)tb_nn(v_loop, 862))->q_kind; ((unsigned char)tb_s376[0] == 101 && (unsigned char)tb_s376[1] == 97 && (unsigned char)tb_s376[2] == 99 && (unsigned char)tb_s376[3] == 104 && tb_s376[4] == 0); })) {
        if (((!f_check__is_name_of(v_i, ((tb_o_Node *)tb_nn(v_loop, 863))->q_name)) || ((!f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 863))->q_kids, 0LL, sizeof(tb_o_Node *), 863)), 863))->q_ty)) && (!f_check__is_dict(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 863))->q_kids, 0LL, sizeof(tb_o_Node *), 863)), 863))->q_ty))))) {
            return;
        }
    } else {
        if ((({ const char *tb_s377 = ((tb_o_Node *)tb_nn(v_i, 866))->q_kind; !((unsigned char)tb_s377[0] == 105 && (unsigned char)tb_s377[1] == 110 && (unsigned char)tb_s377[2] == 100 && (unsigned char)tb_s377[3] == 101 && (unsigned char)tb_s377[4] == 120 && tb_s377[5] == 0); }) || (!f_check__is_name_of((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_i, 866))->q_kids, 1LL, sizeof(tb_o_Node *), 866)), ((tb_o_Node *)tb_nn(v_loop, 866))->q_name)))) {
            return;
        }
        v_plan = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_i, 868))->q_kids, 0LL, sizeof(tb_o_Node *), 868));
        if ((((({ const char *tb_s378 = ((tb_o_Node *)tb_nn(v_plan, 869))->q_kind; !((unsigned char)tb_s378[0] == 110 && (unsigned char)tb_s378[1] == 97 && (unsigned char)tb_s378[2] == 109 && (unsigned char)tb_s378[3] == 101 && tb_s378[4] == 0); }) || (!f_check__declared_before(v_c, ((tb_o_Node *)tb_nn(v_plan, 869))->q_name, ((tb_o_Node *)tb_nn(v_loop, 869))->q_name))) || (!f_check__is_list(v_c, ((tb_o_Node *)tb_nn(v_plan, 869))->q_ty))) || f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_plan, 869))->q_ty))) {
            return;
        }
    }
    {
        long long tb_end379 = ((((tb_o_Node *)tb_nn(v_loop, 871))->q_pre_bases)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end379; v_j++) {
            if (((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 872))->q_pre_bases, v_j, sizeof(tb_o_Node *), 872)), 872))->q_name, ((tb_o_Node *)tb_nn(v_a, 872))->q_name) == 0) && (strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 872))->q_pre_idx, v_j, sizeof(tb_o_Node *), 872)), 872))->q_name, ((tb_o_Node *)tb_nn(v_plan, 872))->q_name) == 0))) {
                return;
            }
        }
    }
    { tb_o_Node * tb_v380 = v_a; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_loop, 874))->q_pre_bases) = tb_v380; }
    { tb_o_Node * tb_v381 = v_plan; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_loop, 875))->q_pre_idx) = tb_v381; }
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
        { _Bool tb_v382 = 1; _Bool *tb_p382 = &(((tb_o_Node *)tb_nn(v_n, 912))->q_creates); *tb_p382 = tb_v382; }
        { long long tb_v383 = v_type; long long *tb_p383 = &(((tb_o_Node *)tb_nn(v_n, 913))->q_ty); *tb_p383 = tb_v383; }
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 914))->q_name, v_type, ((tb_o_Node *)tb_nn(v_n, 914))->q_is_const, 0, ((tb_o_Node *)tb_nn(v_n, 914))->q_line);
        { _Bool tb_v384 = ((tb_o_Node *)tb_nn(v_n, 915))->q_is_shared; _Bool *tb_p384 = &(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 915))->q_vars, ((((tb_o_Comp *)tb_nn(v_c, 915))->q_vars)->len - 1LL), sizeof(tb_o_Var *), 915)), 915))->q_is_shared); *tb_p384 = tb_v384; }
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
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 924))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="), ((tb_o_Node *)tb_nn(v_n, 924))->q_name);
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
    { _Bool tb_v385 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 931))->q_vars, v_v, sizeof(tb_o_Var *), 931)), 931))->q_is_link; _Bool *tb_p385 = &(((tb_o_Node *)tb_nn(v_n, 931))->q_is_link); *tb_p385 = tb_v385; }
    { long long tb_v386 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 932))->q_vars, v_v, sizeof(tb_o_Var *), 932)), 932))->q_type; long long *tb_p386 = &(((tb_o_Node *)tb_nn(v_n, 932))->q_ty); *tb_p386 = tb_v386; }
}

static void f_check__check_set(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 935))->q_kids, 0LL, sizeof(tb_o_Node *), 935));
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 936))->q_value;
    { _Bool tb_v387 = (({ const char *tb_s388 = ((tb_o_Node *)tb_nn(v_target, 937))->q_kind; ((unsigned char)tb_s388[0] == 110 && (unsigned char)tb_s388[1] == 97 && (unsigned char)tb_s388[2] == 109 && (unsigned char)tb_s388[3] == 101 && tb_s388[4] == 0); }) && (({ const char *tb_s389 = v_op; ((unsigned char)tb_s389[0] == 43 && (unsigned char)tb_s389[1] == 61 && tb_s389[2] == 0); }) || ({ const char *tb_s390 = v_op; ((unsigned char)tb_s390[0] == 45 && (unsigned char)tb_s390[1] == 61 && tb_s390[2] == 0); }))); _Bool *tb_p387 = &(((tb_o_Comp *)tb_nn(v_c, 937))->q_shared_ok); *tb_p387 = tb_v387; }
    long long v_to_ty = f_check__check_value(v_c, v_target);
    { _Bool tb_v391 = 0; _Bool *tb_p391 = &(((tb_o_Comp *)tb_nn(v_c, 939))->q_shared_ok); *tb_p391 = tb_v391; }
    f_check__need_place(v_c, v_target, "менять можно только переменную, u.age или nums[i]");
    long long v_r = f_check__root_var(v_c, v_target);
    if (((f_check__outside_temp(v_c, v_r) && ({ const char *tb_s392 = ((tb_o_Node *)tb_nn(v_target, 942))->q_kind; ((unsigned char)tb_s392[0] == 110 && (unsigned char)tb_s392[1] == 97 && (unsigned char)tb_s392[2] == 109 && (unsigned char)tb_s392[3] == 101 && tb_s392[4] == 0); })) && (!f_check__is_plain(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 942))->q_vars, v_r, sizeof(tb_o_Var *), 942)), 942))->q_type)))) {
        f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_n, 943))->q_line, ((tb_o_Node *)tb_nn(v_target, 943))->q_name);
    }
    if ((f_check__outside_par(v_c, v_r) && ((tb_o_Comp *)tb_nn(v_c, 944))->q_par_vec)) {
        if (({ const char *tb_s393 = ((tb_o_Node *)tb_nn(v_target, 945))->q_kind; !((unsigned char)tb_s393[0] == 110 && (unsigned char)tb_s393[1] == 97 && (unsigned char)tb_s393[2] == 109 && (unsigned char)tb_s393[3] == 101 && tb_s393[4] == 0); })) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 946))->q_line, "внутри by vec нельзя менять списки и obj снаружи цикла", "");
        }
        tb_o_Var * v_var = (*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 947))->q_vars, v_r, sizeof(tb_o_Var *), 947));
        if (((({ const char *tb_s394 = v_op; !((unsigned char)tb_s394[0] == 43 && (unsigned char)tb_s394[1] == 61 && tb_s394[2] == 0); }) && ({ const char *tb_s395 = v_op; !((unsigned char)tb_s395[0] == 45 && (unsigned char)tb_s395[1] == 61 && tb_s395[2] == 0); })) || (!f_check__is_number(((tb_o_Var *)tb_nn(v_var, 948))->q_type)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 949))->q_line, "внутри by vec число снаружи можно только копить через += или -=", ((tb_o_Node *)tb_nn(v_target, 949))->q_name);
        }
        _Bool v_found = 0;
        {
            tb_list * tb_l396 = ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 951))->q_par_node, 951))->q_caps;
            for (long long tb_i396 = 0; tb_i396 < tb_l396->len; tb_i396++) {
                tb_o_Var * const v_old = ((tb_o_Var * *)tb_l396->data)[tb_i396];
                if (((strcmp(((tb_o_Var *)tb_nn(v_old, 952))->q_name, ((tb_o_Var *)tb_nn(v_var, 952))->q_name) == 0) && ((tb_o_Var *)tb_nn(v_old, 952))->q_is_shared)) {
                    v_found = 1;
                    if ((strcmp(((tb_o_Var *)tb_nn(v_old, 954))->q_op, v_op) != 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 955))->q_line, "в by vec одну переменную копят одним знаком: или +=, или -=", ((tb_o_Node *)tb_nn(v_target, 955))->q_name);
                    }
                }
            }
        }
        if ((!v_found)) {
            { tb_o_Var * tb_v397 = tb_new_Var(((tb_o_Var *)tb_nn(v_var, 957))->q_name, ((tb_o_Var *)tb_nn(v_var, 957))->q_type, 0, ((tb_o_Var *)tb_nn(v_var, 957))->q_is_link, 0LL, 1, v_op); *(tb_o_Var * *)tb_grow(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 957))->q_par_node, 957))->q_caps) = tb_v397; }
        }
    } else if (f_check__outside_par(v_c, v_r)) {
        if (({ const char *tb_s398 = ((tb_o_Node *)tb_nn(v_target, 959))->q_kind; !((unsigned char)tb_s398[0] == 110 && (unsigned char)tb_s398[1] == 97 && (unsigned char)tb_s398[2] == 109 && (unsigned char)tb_s398[3] == 101 && tb_s398[4] == 0); })) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 960))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)"), "");
        }
        if ((!((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 961))->q_vars, v_r, sizeof(tb_o_Var *), 961)), 961))->q_is_shared)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 962))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="), ((tb_o_Node *)tb_nn(v_target, 962))->q_name);
        }
        if ((({ const char *tb_s399 = v_op; !((unsigned char)tb_s399[0] == 43 && (unsigned char)tb_s399[1] == 61 && tb_s399[2] == 0); }) && ({ const char *tb_s400 = v_op; !((unsigned char)tb_s400[0] == 45 && (unsigned char)tb_s400[1] == 61 && tb_s400[2] == 0); }))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 964))->q_line, "shared внутри on all cores меняется только через += или -=", ((tb_o_Node *)tb_nn(v_target, 964))->q_name);
        }
    }
    long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 965))->q_kids, 1LL, sizeof(tb_o_Node *), 965)));
    { long long tb_v401 = v_to_ty; long long *tb_p401 = &(((tb_o_Node *)tb_nn(v_n, 966))->q_ty); *tb_p401 = tb_v401; }
    if (({ const char *tb_s402 = v_op; ((unsigned char)tb_s402[0] == 61 && tb_s402[1] == 0); })) {
        if ((!f_check__fits(v_to_ty, v_t))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 969))->q_line, "здесь лежит другой тип", f_check__type_name(v_c, v_to_ty));
        }
        return;
    }
    if (((v_to_ty == 4LL) && ({ const char *tb_s403 = v_op; ((unsigned char)tb_s403[0] == 43 && (unsigned char)tb_s403[1] == 61 && tb_s403[2] == 0); }))) {
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
    { long long tb_v404 = v_t; long long *tb_p404 = &(((tb_o_Node *)tb_nn(v_n, 987))->q_ty); *tb_p404 = tb_v404; }
    f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 988))->q_name, v_t, 0, 1, ((tb_o_Node *)tb_nn(v_n, 988))->q_line);
}

static void f_check__check_multi(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 991))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 991))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 991));
    tb_o_Node * v_call = v_value;
    if ((({ const char *tb_s405 = ((tb_o_Node *)tb_nn(v_value, 993))->q_kind; ((unsigned char)tb_s405[0] == 116 && (unsigned char)tb_s405[1] == 114 && (unsigned char)tb_s405[2] == 121 && tb_s405[3] == 0); }) || ({ const char *tb_s406 = ((tb_o_Node *)tb_nn(v_value, 993))->q_kind; ((unsigned char)tb_s406[0] == 101 && (unsigned char)tb_s406[1] == 108 && (unsigned char)tb_s406[2] == 115 && (unsigned char)tb_s406[3] == 101 && (unsigned char)tb_s406[4] == 95 && (unsigned char)tb_s406[5] == 101 && (unsigned char)tb_s406[6] == 114 && (unsigned char)tb_s406[7] == 114 && tb_s406[8] == 0); }))) {
        v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 994))->q_kids, 0LL, sizeof(tb_o_Node *), 994));
    }
    if ((({ const char *tb_s407 = ((tb_o_Node *)tb_nn(v_call, 995))->q_kind; !((unsigned char)tb_s407[0] == 99 && (unsigned char)tb_s407[1] == 97 && (unsigned char)tb_s407[2] == 108 && (unsigned char)tb_s407[3] == 108 && tb_s407[4] == 0); }) && ({ const char *tb_s408 = ((tb_o_Node *)tb_nn(v_call, 995))->q_kind; !((unsigned char)tb_s408[0] == 109 && (unsigned char)tb_s408[1] == 101 && (unsigned char)tb_s408[2] == 116 && (unsigned char)tb_s408[3] == 104 && (unsigned char)tb_s408[4] == 111 && (unsigned char)tb_s408[5] == 100 && tb_s408[6] == 0); }))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 996))->q_line, "справа должен быть вызов функции с gives", "");
    }
    long long v_t = f_check__check_expr(v_c, v_value);
    long long v_names = ((((tb_o_Node *)tb_nn(v_n, 998))->q_kids)->len - 1LL);
    if (({ const char *tb_s409 = f_check__kind_of(v_c, v_t); !((unsigned char)tb_s409[0] == 109 && (unsigned char)tb_s409[1] == 117 && (unsigned char)tb_s409[2] == 108 && (unsigned char)tb_s409[3] == 116 && (unsigned char)tb_s409[4] == 105 && tb_s409[5] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1000))->q_line, "у этой функции один выход, пиши  x = ...", ((tb_o_Node *)tb_nn(v_call, 1000))->q_name);
    }
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1001))->q_types, v_t, sizeof(tb_o_Type *), 1001)), 1001))->q_func_node, 1001))->q_kids, 2LL, sizeof(tb_o_Node *), 1001));
    if (((((tb_o_Node *)tb_nn(v_outputs, 1002))->q_kids)->len != v_names)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1003))->q_line, "у функции другое число выходов", ((tb_o_Node *)tb_nn(v_call, 1003))->q_name);
    }
    {
        long long tb_end410 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end410; v_i++) {
            tb_o_Node * v_name = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1005))->q_kids, v_i, sizeof(tb_o_Node *), 1005));
            long long v_out = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1006))->q_kids, v_i, sizeof(tb_o_Node *), 1006)), 1006))->q_ty;
            long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_name, 1007))->q_name);
            if ((v_v < 0LL)) {
                { _Bool tb_v411 = 1; _Bool *tb_p411 = &(((tb_o_Node *)tb_nn(v_name, 1009))->q_creates); *tb_p411 = tb_v411; }
                f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_name, 1010))->q_name, v_out, 0, 0, ((tb_o_Node *)tb_nn(v_name, 1010))->q_line);
            } else {
                if (f_check__outside_par(v_c, v_v)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_name, 1013))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="), ((tb_o_Node *)tb_nn(v_name, 1013))->q_name);
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
                { _Bool tb_v412 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1020))->q_vars, v_v, sizeof(tb_o_Var *), 1020)), 1020))->q_is_link; _Bool *tb_p412 = &(((tb_o_Node *)tb_nn(v_name, 1020))->q_is_link); *tb_p412 = tb_v412; }
            }
            { long long tb_v413 = v_out; long long *tb_p413 = &(((tb_o_Node *)tb_nn(v_name, 1021))->q_ty); *tb_p413 = tb_v413; }
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
    if ((({ const char *tb_s414 = ((tb_o_Node *)tb_nn(v_f, 1037))->q_name; ((unsigned char)tb_s414[0] == 109 && (unsigned char)tb_s414[1] == 97 && (unsigned char)tb_s414[2] == 105 && (unsigned char)tb_s414[3] == 110 && tb_s414[4] == 0); }) && (v_t != 0LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1038))->q_line, "main ничего не возвращает: пиши просто return", "");
    }
    if ((((tb_o_Node *)tb_nn(v_f, 1039))->q_ty < 0LL)) {
        { long long tb_v415 = v_t; long long *tb_p415 = &(((tb_o_Node *)tb_nn(v_f, 1040))->q_ty); *tb_p415 = tb_v415; }
    } else if (((((tb_o_Node *)tb_nn(v_f, 1041))->q_ty == 1LL) && (v_t == 2LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1042))->q_line, "раньше функция вернула int, а тут dec: сделай оба числа одного типа", "");
    } else if ((!f_check__fits(((tb_o_Node *)tb_nn(v_f, 1043))->q_ty, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1044))->q_line, "раньше эта функция вернула другой тип", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_f, 1044))->q_ty));
    }
}

static void f_check__check_statement(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1047))->q_kind;
    if (({ const char *tb_s416 = v_k; ((unsigned char)tb_s416[0] == 97 && (unsigned char)tb_s416[1] == 115 && (unsigned char)tb_s416[2] == 115 && (unsigned char)tb_s416[3] == 105 && (unsigned char)tb_s416[4] == 103 && (unsigned char)tb_s416[5] == 110 && tb_s416[6] == 0); })) {
        f_check__check_assign(v_c, v_n);
    } else if (({ const char *tb_s417 = v_k; ((unsigned char)tb_s417[0] == 115 && (unsigned char)tb_s417[1] == 101 && (unsigned char)tb_s417[2] == 116 && tb_s417[3] == 0); })) {
        f_check__check_set(v_c, v_n);
    } else if (({ const char *tb_s418 = v_k; ((unsigned char)tb_s418[0] == 108 && (unsigned char)tb_s418[1] == 105 && (unsigned char)tb_s418[2] == 110 && (unsigned char)tb_s418[3] == 107 && (unsigned char)tb_s418[4] == 100 && (unsigned char)tb_s418[5] == 101 && (unsigned char)tb_s418[6] == 99 && (unsigned char)tb_s418[7] == 108 && tb_s418[8] == 0); })) {
        f_check__check_linkdecl(v_c, v_n);
    } else if (({ const char *tb_s419 = v_k; ((unsigned char)tb_s419[0] == 109 && (unsigned char)tb_s419[1] == 117 && (unsigned char)tb_s419[2] == 108 && (unsigned char)tb_s419[3] == 116 && (unsigned char)tb_s419[4] == 105 && tb_s419[5] == 0); })) {
        f_check__check_multi(v_c, v_n);
    } else if (({ const char *tb_s420 = v_k; ((unsigned char)tb_s420[0] == 105 && (unsigned char)tb_s420[1] == 102 && tb_s420[2] == 0); })) {
        long long v_i = 0LL;
        while (((v_i + 1LL) < (((tb_o_Node *)tb_nn(v_n, 1058))->q_kids)->len)) {
            f_check__need_bool(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1059))->q_kids, v_i, sizeof(tb_o_Node *), 1059)));
            f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1060))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 1060)));
            { long long tb_v421 = 2LL; long long *tb_p421 = &(v_i); *tb_p421 += tb_v421; }
        }
        if ((tb_mod((((tb_o_Node *)tb_nn(v_n, 1062))->q_kids)->len, 2LL, 1062) == 1LL)) {
            f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1063))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 1063))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 1063)));
        }
    } else if (({ const char *tb_s422 = v_k; ((unsigned char)tb_s422[0] == 101 && (unsigned char)tb_s422[1] == 97 && (unsigned char)tb_s422[2] == 99 && (unsigned char)tb_s422[3] == 104 && tb_s422[4] == 0); })) {
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
            { tb_o_Node * tb_v423 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1082))->q_loops) = tb_v423; }
            f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1083))->q_kids, 1LL, sizeof(tb_o_Node *), 1083)));
            tb_o_Node * v_gone = (*(tb_o_Node * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 1084))->q_loops, 1084));
            f_check__close_scope(v_c);
        }
    } else if (({ const char *tb_s424 = v_k; ((unsigned char)tb_s424[0] == 99 && (unsigned char)tb_s424[1] == 111 && (unsigned char)tb_s424[2] == 117 && (unsigned char)tb_s424[3] == 110 && (unsigned char)tb_s424[4] == 116 && tb_s424[5] == 0); })) {
        f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1087))->q_kids, 0LL, sizeof(tb_o_Node *), 1087)), "from");
        f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1088))->q_kids, 1LL, sizeof(tb_o_Node *), 1088)), "to");
        f_check__open_scope(v_c);
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1090))->q_name, 1LL, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1090))->q_line);
        { tb_o_Node * tb_v425 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1091))->q_loops) = tb_v425; }
        f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1092))->q_kids, 2LL, sizeof(tb_o_Node *), 1092)));
        tb_o_Node * v_gone = (*(tb_o_Node * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 1093))->q_loops, 1093));
        f_check__close_scope(v_c);
    } else if (((strcmp(v_k, "repeat_times") == 0) || (strcmp(v_k, "repeat_while") == 0))) {
        if ((strcmp(v_k, "repeat_times") == 0)) {
            f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1097))->q_kids, 0LL, sizeof(tb_o_Node *), 1097)), "times");
        } else {
            f_check__need_bool(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1099))->q_kids, 0LL, sizeof(tb_o_Node *), 1099)));
        }
        { long long tb_v426 = 1LL; long long *tb_p426 = &(((tb_o_Comp *)tb_nn(v_c, 1100))->q_loop_depth); *tb_p426 += tb_v426; }
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1101))->q_kids, 1LL, sizeof(tb_o_Node *), 1101)));
        { long long tb_v427 = 1LL; long long *tb_p427 = &(((tb_o_Comp *)tb_nn(v_c, 1102))->q_loop_depth); *tb_p427 -= tb_v427; }
    } else if (({ const char *tb_s428 = v_k; ((unsigned char)tb_s428[0] == 116 && (unsigned char)tb_s428[1] == 111 && (unsigned char)tb_s428[2] == 103 && (unsigned char)tb_s428[3] == 101 && (unsigned char)tb_s428[4] == 116 && (unsigned char)tb_s428[5] == 104 && (unsigned char)tb_s428[6] == 101 && (unsigned char)tb_s428[7] == 114 && tb_s428[8] == 0); })) {
        f_check__check_together(v_c, v_n);
    } else if (({ const char *tb_s429 = v_k; ((unsigned char)tb_s429[0] == 116 && (unsigned char)tb_s429[1] == 101 && (unsigned char)tb_s429[2] == 109 && (unsigned char)tb_s429[3] == 112 && tb_s429[4] == 0); })) {
        long long v_saved_base = ((tb_o_Comp *)tb_nn(v_c, 1106))->q_temp_base;
        long long v_saved_loops = ((tb_o_Comp *)tb_nn(v_c, 1107))->q_temp_loops;
        { long long tb_v430 = (((tb_o_Comp *)tb_nn(v_c, 1108))->q_vars)->len; long long *tb_p430 = &(((tb_o_Comp *)tb_nn(v_c, 1108))->q_temp_base); *tb_p430 = tb_v430; }
        { long long tb_v431 = ((tb_o_Comp *)tb_nn(v_c, 1109))->q_loop_depth; long long *tb_p431 = &(((tb_o_Comp *)tb_nn(v_c, 1109))->q_temp_loops); *tb_p431 = tb_v431; }
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1110))->q_kids, 0LL, sizeof(tb_o_Node *), 1110)));
        { long long tb_v432 = v_saved_base; long long *tb_p432 = &(((tb_o_Comp *)tb_nn(v_c, 1111))->q_temp_base); *tb_p432 = tb_v432; }
        { long long tb_v433 = v_saved_loops; long long *tb_p433 = &(((tb_o_Comp *)tb_nn(v_c, 1112))->q_temp_loops); *tb_p433 = tb_v433; }
    } else if ((({ const char *tb_s434 = v_k; ((unsigned char)tb_s434[0] == 115 && (unsigned char)tb_s434[1] == 107 && (unsigned char)tb_s434[2] == 105 && (unsigned char)tb_s434[3] == 112 && tb_s434[4] == 0); }) || ({ const char *tb_s435 = v_k; ((unsigned char)tb_s435[0] == 115 && (unsigned char)tb_s435[1] == 116 && (unsigned char)tb_s435[2] == 111 && (unsigned char)tb_s435[3] == 112 && tb_s435[4] == 0); }))) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1114))->q_loop_depth == 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1115))->q_line, "skip и stop пишутся только внутри цикла", "");
        }
        if (((({ const char *tb_s436 = v_k; ((unsigned char)tb_s436[0] == 115 && (unsigned char)tb_s436[1] == 116 && (unsigned char)tb_s436[2] == 111 && (unsigned char)tb_s436[3] == 112 && tb_s436[4] == 0); }) && (((tb_o_Comp *)tb_nn(v_c, 1116))->q_par_base >= 0LL)) && (((tb_o_Comp *)tb_nn(v_c, 1116))->q_loop_depth == ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1116))->q_par_node, 1116))->q_temp_no))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1117))->q_line, "stop внутри on all cores нельзя: остальные ядра всё равно работают", "");
        }
        if (((((tb_o_Comp *)tb_nn(v_c, 1118))->q_temp_base >= 0LL) && (((tb_o_Comp *)tb_nn(v_c, 1118))->q_loop_depth == ((tb_o_Comp *)tb_nn(v_c, 1118))->q_temp_loops))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1119))->q_line, "skip и stop внутри temp работают только для циклов внутри temp", "");
        }
    } else if (({ const char *tb_s437 = v_k; ((unsigned char)tb_s437[0] == 114 && (unsigned char)tb_s437[1] == 101 && (unsigned char)tb_s437[2] == 116 && (unsigned char)tb_s437[3] == 117 && (unsigned char)tb_s437[4] == 114 && (unsigned char)tb_s437[5] == 110 && tb_s437[6] == 0); })) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1121))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1122))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " return нельзя"), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1123))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1124))->q_line, "внутри temp return нельзя: сохрани число в переменную снаружи и выйди из блока", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1125))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1126))->q_line, "внутри each ... by N return нельзя: сохрани число в переменную снаружи и выйди через stop", "");
        }
        f_check__check_return(v_c, v_n);
    } else if (({ const char *tb_s438 = v_k; ((unsigned char)tb_s438[0] == 102 && (unsigned char)tb_s438[1] == 97 && (unsigned char)tb_s438[2] == 105 && (unsigned char)tb_s438[3] == 108 && tb_s438[4] == 0); })) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1129))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1130))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " fail нельзя: пиши  or  или  else err"), "");
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
        if ((({ const char *tb_s439 = ((tb_o_Node *)tb_nn(v_e, 1140))->q_kind; ((unsigned char)tb_s439[0] == 116 && (unsigned char)tb_s439[1] == 114 && (unsigned char)tb_s439[2] == 121 && tb_s439[3] == 0); }) || ({ const char *tb_s440 = ((tb_o_Node *)tb_nn(v_e, 1140))->q_kind; ((unsigned char)tb_s440[0] == 101 && (unsigned char)tb_s440[1] == 108 && (unsigned char)tb_s440[2] == 115 && (unsigned char)tb_s440[3] == 101 && (unsigned char)tb_s440[4] == 95 && (unsigned char)tb_s440[5] == 101 && (unsigned char)tb_s440[6] == 114 && (unsigned char)tb_s440[7] == 114 && tb_s440[8] == 0); }))) {
            v_inner = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1141))->q_kids, 0LL, sizeof(tb_o_Node *), 1141));
        }
        if (((({ const char *tb_s441 = ((tb_o_Node *)tb_nn(v_inner, 1142))->q_kind; !((unsigned char)tb_s441[0] == 99 && (unsigned char)tb_s441[1] == 97 && (unsigned char)tb_s441[2] == 108 && (unsigned char)tb_s441[3] == 108 && tb_s441[4] == 0); }) && ({ const char *tb_s442 = ((tb_o_Node *)tb_nn(v_inner, 1142))->q_kind; !((unsigned char)tb_s442[0] == 109 && (unsigned char)tb_s442[1] == 101 && (unsigned char)tb_s442[2] == 116 && (unsigned char)tb_s442[3] == 104 && (unsigned char)tb_s442[4] == 111 && (unsigned char)tb_s442[5] == 100 && tb_s442[6] == 0); })) && (!(({ const char *tb_s443 = ((tb_o_Node *)tb_nn(v_inner, 1142))->q_kind; ((unsigned char)tb_s443[0] == 102 && (unsigned char)tb_s443[1] == 105 && (unsigned char)tb_s443[2] == 101 && (unsigned char)tb_s443[3] == 108 && (unsigned char)tb_s443[4] == 100 && tb_s443[5] == 0); }) && ({ const char *tb_s444 = ((tb_o_Node *)tb_nn(v_inner, 1142))->q_name; ((unsigned char)tb_s444[0] == 119 && (unsigned char)tb_s444[1] == 97 && (unsigned char)tb_s444[2] == 105 && (unsigned char)tb_s444[3] == 116 && tb_s444[4] == 0); }))))) {
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
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1153))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " background нельзя: цикл и так идёт на всех ядрах"), "");
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1154))->q_temp_base >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1155))->q_line, "внутри temp background нельзя: память блока удалится, пока функция ещё работает", "");
    }
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1156))->q_kids, 0LL, sizeof(tb_o_Node *), 1156));
    if ((({ const char *tb_s445 = ((tb_o_Node *)tb_nn(v_call, 1157))->q_kind; ((unsigned char)tb_s445[0] == 99 && (unsigned char)tb_s445[1] == 97 && (unsigned char)tb_s445[2] == 108 && (unsigned char)tb_s445[3] == 108 && tb_s445[4] == 0); }) && f_check__is_builtin(((tb_o_Node *)tb_nn(v_call, 1157))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1158))->q_line, "background запускает свою функцию: a = background load(name)", ((tb_o_Node *)tb_nn(v_call, 1158))->q_name);
    }
    { _Bool tb_v446 = 1; _Bool *tb_p446 = &(((tb_o_Node *)tb_nn(v_call, 1159))->q_creates); *tb_p446 = tb_v446; }
    long long v_t = f_check__check_expr(v_c, v_call);
    if (((({ const char *tb_s447 = ((tb_o_Node *)tb_nn(v_call, 1161))->q_kind; !((unsigned char)tb_s447[0] == 99 && (unsigned char)tb_s447[1] == 97 && (unsigned char)tb_s447[2] == 108 && (unsigned char)tb_s447[3] == 108 && tb_s447[4] == 0); }) || (!((tb_o_Node *)tb_nn(v_call, 1161))->q_has_ref)) || ({ const char *tb_s448 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_call, 1161))->q_ref, 1161))->q_kind; !((unsigned char)tb_s448[0] == 102 && (unsigned char)tb_s448[1] == 117 && (unsigned char)tb_s448[2] == 110 && (unsigned char)tb_s448[3] == 99 && tb_s448[4] == 0); }))) {
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
        tb_list * tb_l449 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1168))->q_kids, 0LL, sizeof(tb_o_Node *), 1168)), 1168))->q_kids;
        for (long long tb_i449 = 0; tb_i449 < tb_l449->len; tb_i449++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l449->data)[tb_i449];
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
        tb_list * tb_l450 = ((tb_o_Node *)tb_nn(v_block, 1180))->q_kids;
        for (long long tb_i450 = 0; tb_i450 < tb_l450->len; tb_i450++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l450->data)[tb_i450];
            if ((({ const char *tb_s451 = ((tb_o_Node *)tb_nn(v_s, 1181))->q_kind; !((unsigned char)tb_s451[0] == 97 && (unsigned char)tb_s451[1] == 115 && (unsigned char)tb_s451[2] == 115 && (unsigned char)tb_s451[3] == 105 && (unsigned char)tb_s451[4] == 103 && (unsigned char)tb_s451[5] == 110 && tb_s451[6] == 0); }) || ((((tb_o_Node *)tb_nn(v_s, 1181))->q_kids)->len != 1LL))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1182))->q_line, "в together пишутся только новые переменные со значением: a = d[key]", "");
            }
            if ((f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_s, 1183))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1184))->q_line, "в together заводятся только новые переменные, а эта уже есть", ((tb_o_Node *)tb_nn(v_s, 1184))->q_name);
            }
            {
                tb_list * tb_l452 = v_names;
                for (long long tb_i452 = 0; tb_i452 < tb_l452->len; tb_i452++) {
                    const char * const v_w = ((const char * *)tb_l452->data)[tb_i452];
                    if ((strcmp(v_w, ((tb_o_Node *)tb_nn(v_s, 1186))->q_name) == 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1187))->q_line, "в together эта переменная уже заведена строкой выше", ((tb_o_Node *)tb_nn(v_s, 1187))->q_name);
                    }
                    if (f_check__uses_name((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_s, 1188))->q_kids, 0LL, sizeof(tb_o_Node *), 1188)), v_w)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1189))->q_line, "строки в together не должны зависеть друг от друга, а здесь нужна", v_w);
                    }
                }
            }
            { const char * tb_v453 = ((tb_o_Node *)tb_nn(v_s, 1190))->q_name; *(const char * *)tb_grow(v_names) = tb_v453; }
            f_check__check_statement(v_c, v_s);
        }
    }
}

static _Bool f_check__uses_name(tb_o_Node * v_n, const char * v_name) {
    if ((({ const char *tb_s454 = ((tb_o_Node *)tb_nn(v_n, 1195))->q_kind; ((unsigned char)tb_s454[0] == 110 && (unsigned char)tb_s454[1] == 97 && (unsigned char)tb_s454[2] == 109 && (unsigned char)tb_s454[3] == 101 && tb_s454[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1195))->q_name, v_name) == 0))) {
        return 1;
    }
    {
        tb_list * tb_l455 = ((tb_o_Node *)tb_nn(v_n, 1197))->q_kids;
        for (long long tb_i455 = 0; tb_i455 < tb_l455->len; tb_i455++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l455->data)[tb_i455];
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
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1212))->q_line, tb_cat(tb_cat("each ... by N внутри ", f_check__loop_word(v_c)), " пока нельзя"), "");
    }
    f_check__need_int(v_c, ((tb_o_Node *)tb_nn(v_n, 1213))->q_piece_size, "by");
    { const char * tb_v456 = ((tb_o_Comp *)tb_nn(v_c, 1214))->q_file_name; const char * *tb_p456 = &(((tb_o_Node *)tb_nn(v_n, 1214))->q_file); *tb_p456 = tb_v456; }
    { tb_o_Node * tb_v457 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1215))->q_pieces) = tb_v457; }
    f_check__open_scope(v_c);
    f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1217))->q_name, v_t, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1217))->q_line);
    { long long tb_v458 = 1LL; long long *tb_p458 = &(((tb_o_Comp *)tb_nn(v_c, 1218))->q_piece_depth); *tb_p458 += tb_v458; }
    { tb_o_Node * tb_v459 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1219))->q_loops) = tb_v459; }
    f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1220))->q_kids, 1LL, sizeof(tb_o_Node *), 1220)));
    tb_o_Node * v_gone = (*(tb_o_Node * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 1221))->q_loops, 1221));
    { long long tb_v460 = 1LL; long long *tb_p460 = &(((tb_o_Comp *)tb_nn(v_c, 1222))->q_piece_depth); *tb_p460 -= tb_v460; }
    f_check__close_scope(v_c);
}

static _Bool f_check__is_name_of(tb_o_Node * v_v, const char * v_name) {
    return (({ const char *tb_s461 = ((tb_o_Node *)tb_nn(v_v, 1227))->q_kind; ((unsigned char)tb_s461[0] == 110 && (unsigned char)tb_s461[1] == 97 && (unsigned char)tb_s461[2] == 109 && (unsigned char)tb_s461[3] == 101 && tb_s461[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_v, 1227))->q_name, v_name) == 0));
    tb_crash(1226, "функция is_name_of дошла до конца без return");
    return 0;
}

static long long f_check__piece_leak(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_name) {
    long long v_r = 0LL;
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1233))->q_kind;
    if ((({ const char *tb_s462 = v_k; ((unsigned char)tb_s462[0] == 110 && (unsigned char)tb_s462[1] == 97 && (unsigned char)tb_s462[2] == 109 && (unsigned char)tb_s462[3] == 101 && tb_s462[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1234))->q_name, v_name) == 0))) {
        return ((tb_o_Node *)tb_nn(v_n, 1235))->q_line;
    }
    if (((({ const char *tb_s463 = v_k; ((unsigned char)tb_s463[0] == 97 && (unsigned char)tb_s463[1] == 115 && (unsigned char)tb_s463[2] == 115 && (unsigned char)tb_s463[3] == 105 && (unsigned char)tb_s463[4] == 103 && (unsigned char)tb_s463[5] == 110 && tb_s463[6] == 0); }) || ({ const char *tb_s464 = v_k; ((unsigned char)tb_s464[0] == 108 && (unsigned char)tb_s464[1] == 105 && (unsigned char)tb_s464[2] == 110 && (unsigned char)tb_s464[3] == 107 && (unsigned char)tb_s464[4] == 100 && (unsigned char)tb_s464[5] == 101 && (unsigned char)tb_s464[6] == 99 && (unsigned char)tb_s464[7] == 108 && tb_s464[8] == 0); })) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1236))->q_name, v_name) == 0))) {
        return ((tb_o_Node *)tb_nn(v_n, 1237))->q_line;
    }
    if (((({ const char *tb_s465 = v_k; ((unsigned char)tb_s465[0] == 99 && (unsigned char)tb_s465[1] == 97 && (unsigned char)tb_s465[2] == 108 && (unsigned char)tb_s465[3] == 108 && tb_s465[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1238))->q_has_ref) && ({ const char *tb_s466 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1238))->q_ref, 1238))->q_kind; ((unsigned char)tb_s466[0] == 102 && (unsigned char)tb_s466[1] == 117 && (unsigned char)tb_s466[2] == 110 && (unsigned char)tb_s466[3] == 99 && tb_s466[4] == 0); }))) {
        {
            long long tb_end467 = ((((tb_o_Node *)tb_nn(v_n, 1239))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end467; v_i++) {
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
    if ((({ const char *tb_s468 = v_k; ((unsigned char)tb_s468[0] == 99 && (unsigned char)tb_s468[1] == 97 && (unsigned char)tb_s468[2] == 108 && (unsigned char)tb_s468[3] == 108 && tb_s468[4] == 0); }) && f_check__is_builtin(((tb_o_Node *)tb_nn(v_n, 1249))->q_name))) {
        {
            tb_list * tb_l469 = ((tb_o_Node *)tb_nn(v_n, 1250))->q_kids;
            for (long long tb_i469 = 0; tb_i469 < tb_l469->len; tb_i469++) {
                tb_o_Node * const v_a = ((tb_o_Node * *)tb_l469->data)[tb_i469];
                tb_o_Node * v_v = v_a;
                if (({ const char *tb_s470 = ((tb_o_Node *)tb_nn(v_a, 1252))->q_kind; ((unsigned char)tb_s470[0] == 97 && (unsigned char)tb_s470[1] == 114 && (unsigned char)tb_s470[2] == 103 && tb_s470[3] == 0); })) {
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
    if ((((({ const char *tb_s471 = v_k; ((unsigned char)tb_s471[0] == 105 && (unsigned char)tb_s471[1] == 110 && (unsigned char)tb_s471[2] == 100 && (unsigned char)tb_s471[3] == 101 && (unsigned char)tb_s471[4] == 120 && tb_s471[5] == 0); }) || ({ const char *tb_s472 = v_k; ((unsigned char)tb_s472[0] == 101 && (unsigned char)tb_s472[1] == 97 && (unsigned char)tb_s472[2] == 99 && (unsigned char)tb_s472[3] == 104 && tb_s472[4] == 0); })) && ((((tb_o_Node *)tb_nn(v_n, 1260))->q_kids)->len > 0LL)) && f_check__is_name_of((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1260))->q_kids, 0LL, sizeof(tb_o_Node *), 1260)), v_name))) {
        v_start = 1LL;
    }
    if ((({ const char *tb_s473 = v_k; ((unsigned char)tb_s473[0] == 101 && (unsigned char)tb_s473[1] == 97 && (unsigned char)tb_s473[2] == 99 && (unsigned char)tb_s473[3] == 104 && tb_s473[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1262))->q_by_piece)) {
        long long v_bad = f_check__piece_leak(v_c, ((tb_o_Node *)tb_nn(v_n, 1263))->q_piece_size, v_name);
        if ((v_bad != 0LL)) {
            return v_bad;
        }
    }
    {
        long long tb_end474 = ((((tb_o_Node *)tb_nn(v_n, 1266))->q_kids)->len - 1LL);
        for (long long v_i = v_start; v_i <= tb_end474; v_i++) {
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
    const char * v_key = tb_cat(tb_cat(tb_cat(tb_cat(((tb_o_Node *)tb_nn(v_f, 1274))->q_name, "@"), ((tb_o_Node *)tb_nn(v_f, 1274))->q_mod), "#"), tb_itos(v_i));
    {
        long long tb_end475 = ((((tb_o_Comp *)tb_nn(v_c, 1275))->q_safe_keys)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end475; v_j++) {
            if ((strcmp((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1276))->q_safe_keys, v_j, sizeof(const char *), 1276)), v_key) == 0)) {
                return ((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1277))->q_safe_vals, v_j, sizeof(long long), 1277)) == 2LL);
            }
        }
    }
    { const char * tb_v476 = v_key; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1279))->q_safe_keys) = tb_v476; }
    { long long tb_v477 = 1LL; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1280))->q_safe_vals) = tb_v477; }
    long long v_slot = ((((tb_o_Comp *)tb_nn(v_c, 1281))->q_safe_keys)->len - 1LL);
    { long long tb_v478 = 1LL; long long *tb_p478 = &(((tb_o_Comp *)tb_nn(v_c, 1282))->q_safe_busy); *tb_p478 += tb_v478; }
    long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1283))->q_kids, 1LL, sizeof(tb_o_Node *), 1283)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1283))->q_kids, 0LL, sizeof(tb_o_Node *), 1283)), 1283))->q_kids, v_i, sizeof(tb_o_Node *), 1283)), 1283))->q_name);
    { long long tb_v479 = 1LL; long long *tb_p479 = &(((tb_o_Comp *)tb_nn(v_c, 1284))->q_safe_busy); *tb_p479 -= tb_v479; }
    if ((v_bad != 0LL)) {
        { long long tb_v480 = 2LL; long long *tb_p480 = &((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1286))->q_safe_vals, v_slot, sizeof(long long), 1286))); *tb_p480 = tb_v480; }
        return 1;
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1288))->q_safe_busy > 0LL)) {
        { long long tb_v481 = 3LL; long long *tb_p481 = &((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1290))->q_safe_vals, v_slot, sizeof(long long), 1290))); *tb_p481 = tb_v481; }
        { const char * tb_v482 = ""; const char * *tb_p482 = &((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1291))->q_safe_keys, v_slot, sizeof(const char *), 1291))); *tb_p482 = tb_v482; }
    }
    return 0;
    tb_crash(1273, "функция param_keeps_piece дошла до конца без return");
    return 0;
}

static void f_check__mark_hot_fields(tb_o_Comp * v_c) {
    tb_list * v_hot_func = tb_bits_new();
    {
        long long tb_end483 = (((tb_o_Comp *)tb_nn(v_c, 1301))->q_funcs)->len;
        for (long long v_i = 1LL; v_i <= tb_end483; v_i++) {
            { _Bool tb_v484 = 0; tb_badd(v_hot_func, tb_v484); }
        }
    }
    {
        long long tb_end485 = ((((tb_o_Comp *)tb_nn(v_c, 1303))->q_call_to)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end485; v_j++) {
            if (tb_bget(((tb_o_Comp *)tb_nn(v_c, 1304))->q_call_loop, v_j, 1304)) {
                { _Bool tb_v486 = 1; tb_bset(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1305))->q_call_to, v_j, sizeof(long long), 1305)), tb_v486, 1305); }
            }
        }
    }
    _Bool v_changed = 1;
    while (v_changed) {
        v_changed = 0;
        {
            long long tb_end487 = ((((tb_o_Comp *)tb_nn(v_c, 1309))->q_call_to)->len - 1LL);
            for (long long v_j = 0LL; v_j <= tb_end487; v_j++) {
                if (((((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1310))->q_call_from, v_j, sizeof(long long), 1310)) >= 0LL) && tb_bget(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1310))->q_call_from, v_j, sizeof(long long), 1310)), 1310)) && (!tb_bget(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1310))->q_call_to, v_j, sizeof(long long), 1310)), 1310)))) {
                    { _Bool tb_v488 = 1; tb_bset(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1311))->q_call_to, v_j, sizeof(long long), 1311)), tb_v488, 1311); }
                    v_changed = 1;
                }
            }
        }
    }
    {
        long long tb_end489 = ((((tb_o_Comp *)tb_nn(v_c, 1313))->q_acc_field)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end489; v_j++) {
            if ((tb_bget(((tb_o_Comp *)tb_nn(v_c, 1314))->q_acc_loop, v_j, 1314) || (((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1314))->q_acc_func, v_j, sizeof(long long), 1314)) >= 0LL) && tb_bget(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1314))->q_acc_func, v_j, sizeof(long long), 1314)), 1314)))) {
                { _Bool tb_v490 = 1; _Bool *tb_p490 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1315))->q_acc_field, v_j, sizeof(tb_o_Node *), 1315)), 1315))->q_hot); *tb_p490 = tb_v490; }
            }
        }
    }
    {
        tb_list * tb_l491 = ((tb_o_Comp *)tb_nn(v_c, 1316))->q_objs;
        for (long long tb_i491 = 0; tb_i491 < tb_l491->len; tb_i491++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l491->data)[tb_i491];
            if (((tb_o_Node *)tb_nn(v_o, 1317))->q_by_cols) {
                continue;
            }
            long long v_hot = 0LL;
            long long v_cold_bytes = 0LL;
            {
                tb_list * tb_l492 = ((tb_o_Node *)tb_nn(v_o, 1321))->q_kids;
                for (long long tb_i492 = 0; tb_i492 < tb_l492->len; tb_i492++) {
                    tb_o_Node * const v_f = ((tb_o_Node * *)tb_l492->data)[tb_i492];
                    if (((tb_o_Node *)tb_nn(v_f, 1322))->q_hot) {
                        { long long tb_v493 = 1LL; long long *tb_p493 = &(v_hot); *tb_p493 += tb_v493; }
                    } else if ((((tb_o_Node *)tb_nn(v_f, 1324))->q_ty == 3LL)) {
                        { long long tb_v494 = 1LL; long long *tb_p494 = &(v_cold_bytes); *tb_p494 += tb_v494; }
                    } else {
                        { long long tb_v495 = 8LL; long long *tb_p495 = &(v_cold_bytes); *tb_p495 += tb_v495; }
                    }
                }
            }
            if (((v_hot > 0LL) && (v_cold_bytes >= 16LL))) {
                {
                    tb_list * tb_l496 = ((tb_o_Node *)tb_nn(v_o, 1329))->q_kids;
                    for (long long tb_i496 = 0; tb_i496 < tb_l496->len; tb_i496++) {
                        tb_o_Node * const v_f = ((tb_o_Node * *)tb_l496->data)[tb_i496];
                        { _Bool tb_v497 = (!((tb_o_Node *)tb_nn(v_f, 1330))->q_hot); _Bool *tb_p497 = &(((tb_o_Node *)tb_nn(v_f, 1330))->q_is_cold); *tb_p497 = tb_v497; }
                    }
                }
            }
        }
    }
}

static void f_check__check_piece_leaks(tb_o_Comp * v_c) {
    {
        tb_list * tb_l498 = ((tb_o_Comp *)tb_nn(v_c, 1334))->q_pieces;
        for (long long tb_i498 = 0; tb_i498 < tb_l498->len; tb_i498++) {
            tb_o_Node * const v_n = ((tb_o_Node * *)tb_l498->data)[tb_i498];
            long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1335))->q_kids, 1LL, sizeof(tb_o_Node *), 1335)), ((tb_o_Node *)tb_nn(v_n, 1335))->q_name);
            if ((v_bad != 0LL)) {
                { const char * tb_v499 = ((tb_o_Node *)tb_nn(v_n, 1337))->q_file; const char * *tb_p499 = &(((tb_o_Comp *)tb_nn(v_c, 1337))->q_file_name); *tb_p499 = tb_v499; }
                f_errors__error_at(v_c, v_bad, "кусок живёт только внутри своего цикла: его можно читать и менять по номеру, ходить по нему each и передавать в функцию, но нельзя сохранять, растить .add и уменьшать .pop", ((tb_o_Node *)tb_nn(v_n, 1338))->q_name);
            }
        }
    }
}

static void f_check__check_loop_body(tb_o_Comp * v_c, tb_o_Node * v_n, tb_o_Node * v_body) {
    { long long tb_v500 = 1LL; long long *tb_p500 = &(((tb_o_Comp *)tb_nn(v_c, 1343))->q_loop_depth); *tb_p500 += tb_v500; }
    if ((((tb_o_Node *)tb_nn(v_n, 1344))->q_is_par || ((tb_o_Node *)tb_nn(v_n, 1344))->q_by_vec)) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1345))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1346))->q_line, "on all cores и by vec внутри другого on all cores или by vec нельзя", "");
        }
        { long long tb_v501 = ((((tb_o_Comp *)tb_nn(v_c, 1347))->q_vars)->len - 1LL); long long *tb_p501 = &(((tb_o_Comp *)tb_nn(v_c, 1347))->q_par_base); *tb_p501 = tb_v501; }
        { tb_o_Node * tb_v502 = v_n; tb_o_Node * *tb_p502 = &(((tb_o_Comp *)tb_nn(v_c, 1348))->q_par_node); *tb_p502 = tb_v502; }
        { _Bool tb_v503 = ((tb_o_Node *)tb_nn(v_n, 1349))->q_by_vec; _Bool *tb_p503 = &(((tb_o_Comp *)tb_nn(v_c, 1349))->q_par_vec); *tb_p503 = tb_v503; }
        { long long tb_v504 = ((tb_o_Comp *)tb_nn(v_c, 1351))->q_loop_depth; long long *tb_p504 = &(((tb_o_Node *)tb_nn(v_n, 1351))->q_temp_no); *tb_p504 = tb_v504; }
        f_check__check_block(v_c, v_body);
        { long long tb_v505 = (-1LL); long long *tb_p505 = &(((tb_o_Comp *)tb_nn(v_c, 1353))->q_par_base); *tb_p505 = tb_v505; }
        { _Bool tb_v506 = 0; _Bool *tb_p506 = &(((tb_o_Comp *)tb_nn(v_c, 1354))->q_par_vec); *tb_p506 = tb_v506; }
        if (((tb_o_Node *)tb_nn(v_n, 1355))->q_by_vec) {
            {
                tb_list * tb_l507 = ((tb_o_Node *)tb_nn(v_n, 1357))->q_caps;
                for (long long tb_i507 = 0; tb_i507 < tb_l507->len; tb_i507++) {
                    tb_o_Var * const v_a = ((tb_o_Var * *)tb_l507->data)[tb_i507];
                    {
                        tb_list * tb_l508 = ((tb_o_Node *)tb_nn(v_n, 1358))->q_caps;
                        for (long long tb_i508 = 0; tb_i508 < tb_l508->len; tb_i508++) {
                            tb_o_Var * const v_b = ((tb_o_Var * *)tb_l508->data)[tb_i508];
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
    { long long tb_v509 = 1LL; long long *tb_p509 = &(((tb_o_Comp *)tb_nn(v_c, 1363))->q_loop_depth); *tb_p509 -= tb_v509; }
}

static void f_check__check_block(tb_o_Comp * v_c, tb_o_Node * v_block) {
    f_check__open_scope(v_c);
    {
        tb_list * tb_l510 = ((tb_o_Node *)tb_nn(v_block, 1367))->q_kids;
        for (long long tb_i510 = 0; tb_i510 < tb_l510->len; tb_i510++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l510->data)[tb_i510];
            f_check__check_statement(v_c, v_s);
        }
    }
    f_check__close_scope(v_c);
}

static void f_check__check_func(tb_o_Comp * v_c, long long v_fi) {
    tb_o_Func * v_f = (*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1372))->q_funcs, v_fi, sizeof(tb_o_Func *), 1372));
    { long long tb_v511 = v_fi; long long *tb_p511 = &(((tb_o_Comp *)tb_nn(v_c, 1373))->q_current); *tb_p511 = tb_v511; }
    { const char * tb_v512 = ((tb_o_Func *)tb_nn(v_f, 1374))->q_mod; const char * *tb_p512 = &(((tb_o_Comp *)tb_nn(v_c, 1374))->q_cur_mod); *tb_p512 = tb_v512; }
    { const char * tb_v513 = ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1375))->q_node, 1375))->q_file; const char * *tb_p513 = &(((tb_o_Comp *)tb_nn(v_c, 1375))->q_file_name); *tb_p513 = tb_v513; }
    { long long tb_v514 = 1LL; long long *tb_p514 = &(((tb_o_Func *)tb_nn(v_f, 1376))->q_state); *tb_p514 = tb_v514; }
    { long long tb_v515 = 0LL; long long *tb_p515 = &(((tb_o_Comp *)tb_nn(v_c, 1377))->q_loop_depth); *tb_p515 = tb_v515; }
    f_check__open_scope(v_c);
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1379))->q_node, 1379))->q_kids, 0LL, sizeof(tb_o_Node *), 1379));
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1380))->q_node, 1380))->q_kids, 2LL, sizeof(tb_o_Node *), 1380));
    {
        tb_list * tb_l516 = ((tb_o_Node *)tb_nn(v_params, 1381))->q_kids;
        for (long long tb_i516 = 0; tb_i516 < tb_l516->len; tb_i516++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l516->data)[tb_i516];
            f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_p, 1382))->q_name, ((tb_o_Node *)tb_nn(v_p, 1382))->q_ty, 0, ((tb_o_Node *)tb_nn(v_p, 1382))->q_is_link, ((tb_o_Node *)tb_nn(v_p, 1382))->q_line);
        }
    }
    {
        tb_list * tb_l517 = ((tb_o_Node *)tb_nn(v_outputs, 1383))->q_kids;
        for (long long tb_i517 = 0; tb_i517 < tb_l517->len; tb_i517++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l517->data)[tb_i517];
            long long v_same_param = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_o, 1384))->q_name);
            if ((v_same_param >= 0LL)) {
                if ((((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1387))->q_vars, v_same_param, sizeof(tb_o_Var *), 1387)), 1387))->q_type != ((tb_o_Node *)tb_nn(v_o, 1387))->q_ty)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1388))->q_line, "выход и параметр с одним именем должны быть одного типа", ((tb_o_Node *)tb_nn(v_o, 1388))->q_name);
                }
                { _Bool tb_v518 = 1; _Bool *tb_p518 = &(((tb_o_Node *)tb_nn(v_o, 1390))->q_is_const); *tb_p518 = tb_v518; }
            } else {
                f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_o, 1392))->q_name, ((tb_o_Node *)tb_nn(v_o, 1392))->q_ty, 0, 0, ((tb_o_Node *)tb_nn(v_o, 1392))->q_line);
            }
        }
    }
    f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1393))->q_node, 1393))->q_kids, 1LL, sizeof(tb_o_Node *), 1393)));
    f_check__close_scope(v_c);
    if ((((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1395))->q_node, 1395))->q_ty < 0LL)) {
        { long long tb_v519 = 0LL; long long *tb_p519 = &(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1396))->q_node, 1396))->q_ty); *tb_p519 = tb_v519; }
    }
    { long long tb_v520 = 2LL; long long *tb_p520 = &(((tb_o_Func *)tb_nn(v_f, 1397))->q_state); *tb_p520 = tb_v520; }
}

static void f_check__register_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_o, 1400))->q_name) >= 0LL) || f_check__is_builtin(((tb_o_Node *)tb_nn(v_o, 1400))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1401))->q_line, "это имя уже занято", ((tb_o_Node *)tb_nn(v_o, 1401))->q_name);
    }
    if (((((tb_o_Comp *)tb_nn(v_c, 1402))->q_objs)->len == 500LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1403))->q_line, "слишком много obj", "");
    }
    { tb_o_Node * tb_v521 = v_o; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1404))->q_objs) = tb_v521; }
}

static void f_check__register_func(tb_o_Comp * v_c, tb_o_Node * v_fn) {
    if ((((f_check__find_func_in(v_c, ((tb_o_Node *)tb_nn(v_fn, 1407))->q_name, ((tb_o_Node *)tb_nn(v_fn, 1407))->q_mod) >= 0LL) || (f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_fn, 1407))->q_name) >= 0LL)) || f_check__is_builtin(((tb_o_Node *)tb_nn(v_fn, 1407))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_fn, 1408))->q_line, "это имя уже занято", ((tb_o_Node *)tb_nn(v_fn, 1408))->q_name);
    }
    if (((((tb_o_Comp *)tb_nn(v_c, 1409))->q_funcs)->len == 1000LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_fn, 1410))->q_line, "слишком много функций", "");
    }
    { tb_o_Func * tb_v522 = tb_new_Func(((tb_o_Node *)tb_nn(v_fn, 1411))->q_name, ((tb_o_Node *)tb_nn(v_fn, 1411))->q_mod, v_fn, 0LL); *(tb_o_Func * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1411))->q_funcs) = tb_v522; }
}

static _Bool f_check__has_fail(tb_o_Node * v_n) {
    _Bool v_r = 0;
    if ((({ const char *tb_s523 = ((tb_o_Node *)tb_nn(v_n, 1415))->q_kind; ((unsigned char)tb_s523[0] == 102 && (unsigned char)tb_s523[1] == 97 && (unsigned char)tb_s523[2] == 105 && (unsigned char)tb_s523[3] == 108 && tb_s523[4] == 0); }) || ({ const char *tb_s524 = ((tb_o_Node *)tb_nn(v_n, 1415))->q_kind; ((unsigned char)tb_s524[0] == 116 && (unsigned char)tb_s524[1] == 114 && (unsigned char)tb_s524[2] == 121 && tb_s524[3] == 0); }))) {
        return 1;
    }
    {
        tb_list * tb_l525 = ((tb_o_Node *)tb_nn(v_n, 1417))->q_kids;
        for (long long tb_i525 = 0; tb_i525 < tb_l525->len; tb_i525++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l525->data)[tb_i525];
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
    if ((({ const char *tb_s526 = ((tb_o_Node *)tb_nn(v_n, 1424))->q_kind; ((unsigned char)tb_s526[0] == 114 && (unsigned char)tb_s526[1] == 101 && (unsigned char)tb_s526[2] == 116 && (unsigned char)tb_s526[3] == 117 && (unsigned char)tb_s526[4] == 114 && (unsigned char)tb_s526[5] == 110 && tb_s526[6] == 0); }) && ((((tb_o_Node *)tb_nn(v_n, 1424))->q_kids)->len > 0LL))) {
        return 1;
    }
    {
        tb_list * tb_l527 = ((tb_o_Node *)tb_nn(v_n, 1426))->q_kids;
        for (long long tb_i527 = 0; tb_i527 < tb_l527->len; tb_i527++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l527->data)[tb_i527];
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
        tb_list * tb_l528 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_fn, 1432))->q_kids, 0LL, sizeof(tb_o_Node *), 1432)), 1432))->q_kids;
        for (long long tb_i528 = 0; tb_i528 < tb_l528->len; tb_i528++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l528->data)[tb_i528];
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
        long long tb_end529 = ((((tb_o_Node *)tb_nn(v_params, 1438))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end529; v_i++) {
            tb_o_Node * v_p = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1439))->q_kids, v_i, sizeof(tb_o_Node *), 1439));
            {
                long long tb_end530 = (v_i - 1LL);
                for (long long v_j = 0LL; v_j <= tb_end530; v_j++) {
                    if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1441))->q_kids, v_j, sizeof(tb_o_Node *), 1441)), 1441))->q_name, ((tb_o_Node *)tb_nn(v_p, 1441))->q_name) == 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_p, 1442))->q_line, "это имя уже есть в списке", ((tb_o_Node *)tb_nn(v_p, 1442))->q_name);
                    }
                }
            }
            if (((tb_o_Node *)tb_nn(v_p, 1443))->q_has_tsyn) {
                { long long tb_v531 = f_check__resolve_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1444))->q_tsyn); long long *tb_p531 = &(((tb_o_Node *)tb_nn(v_p, 1444))->q_ty); *tb_p531 = tb_v531; }
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
    { long long tb_v532 = (-1LL); *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1452))->q_temp_types) = tb_v532; }
    { long long tb_v533 = (-1LL); long long *tb_p533 = &(((tb_o_Comp *)tb_nn(v_c, 1453))->q_par_base); *tb_p533 = tb_v533; }
    { long long tb_v534 = (-1LL); long long *tb_p534 = &(((tb_o_Comp *)tb_nn(v_c, 1454))->q_temp_base); *tb_p534 = tb_v534; }
    const char * v_main_mod = "";
    if (((((tb_o_Node *)tb_nn(v_program, 1458))->q_kids)->len > 0LL)) {
        v_main_mod = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1459))->q_kids, 0LL, sizeof(tb_o_Node *), 1459)), 1459))->q_mod;
    }
    {
        tb_list * tb_l535 = ((tb_o_Node *)tb_nn(v_program, 1460))->q_kids;
        for (long long tb_i535 = 0; tb_i535 < tb_l535->len; tb_i535++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l535->data)[tb_i535];
            if ((!f_check__is_module(v_c, ((tb_o_Node *)tb_nn(v_top, 1461))->q_mod))) {
                { const char * tb_v536 = ((tb_o_Node *)tb_nn(v_top, 1462))->q_mod; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1462))->q_all_mods) = tb_v536; }
            }
            if (({ const char *tb_s537 = ((tb_o_Node *)tb_nn(v_top, 1463))->q_kind; ((unsigned char)tb_s537[0] == 117 && (unsigned char)tb_s537[1] == 115 && (unsigned char)tb_s537[2] == 101 && tb_s537[3] == 0); })) {
                if (((((tb_o_Comp *)tb_nn(v_c, 1464))->q_uses)->len == 500LL)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_top, 1465))->q_line, "слишком много use", "");
                }
                { tb_o_Use * tb_v538 = tb_new_Use(((tb_o_Node *)tb_nn(v_top, 1466))->q_mod, ((tb_o_Node *)tb_nn(v_top, 1466))->q_name); *(tb_o_Use * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1466))->q_uses) = tb_v538; }
            }
        }
    }
    {
        tb_list * tb_l539 = ((tb_o_Node *)tb_nn(v_program, 1468))->q_kids;
        for (long long tb_i539 = 0; tb_i539 < tb_l539->len; tb_i539++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l539->data)[tb_i539];
            { const char * tb_v540 = ((tb_o_Node *)tb_nn(v_top, 1469))->q_file; const char * *tb_p540 = &(((tb_o_Comp *)tb_nn(v_c, 1469))->q_file_name); *tb_p540 = tb_v540; }
            if (({ const char *tb_s541 = ((tb_o_Node *)tb_nn(v_top, 1470))->q_kind; ((unsigned char)tb_s541[0] == 111 && (unsigned char)tb_s541[1] == 98 && (unsigned char)tb_s541[2] == 106 && tb_s541[3] == 0); })) {
                f_check__register_obj(v_c, v_top);
            } else if (({ const char *tb_s542 = ((tb_o_Node *)tb_nn(v_top, 1472))->q_kind; ((unsigned char)tb_s542[0] == 102 && (unsigned char)tb_s542[1] == 117 && (unsigned char)tb_s542[2] == 110 && (unsigned char)tb_s542[3] == 99 && tb_s542[4] == 0); })) {
                f_check__register_func(v_c, v_top);
                { _Bool tb_v543 = f_check__has_fail((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1474))->q_kids, 1LL, sizeof(tb_o_Node *), 1474))); _Bool *tb_p543 = &(((tb_o_Node *)tb_nn(v_top, 1474))->q_can_fail); *tb_p543 = tb_v543; }
                if ((({ const char *tb_s544 = ((tb_o_Node *)tb_nn(v_top, 1475))->q_name; ((unsigned char)tb_s544[0] == 109 && (unsigned char)tb_s544[1] == 97 && (unsigned char)tb_s544[2] == 105 && (unsigned char)tb_s544[3] == 110 && tb_s544[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_top, 1475))->q_mod, v_main_mod) != 0))) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_top, 1476))->q_line, "main бывает только в главном файле, в модуле её быть не должно", "");
                }
            }
        }
    }
    {
        tb_list * tb_l545 = ((tb_o_Node *)tb_nn(v_program, 1478))->q_kids;
        for (long long tb_i545 = 0; tb_i545 < tb_l545->len; tb_i545++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l545->data)[tb_i545];
            { const char * tb_v546 = ((tb_o_Node *)tb_nn(v_top, 1479))->q_file; const char * *tb_p546 = &(((tb_o_Comp *)tb_nn(v_c, 1479))->q_file_name); *tb_p546 = tb_v546; }
            if (({ const char *tb_s547 = ((tb_o_Node *)tb_nn(v_top, 1480))->q_kind; ((unsigned char)tb_s547[0] == 111 && (unsigned char)tb_s547[1] == 98 && (unsigned char)tb_s547[2] == 106 && tb_s547[3] == 0); })) {
                f_check__resolve_params(v_c, v_top);
            } else if (({ const char *tb_s548 = ((tb_o_Node *)tb_nn(v_top, 1482))->q_kind; ((unsigned char)tb_s548[0] == 102 && (unsigned char)tb_s548[1] == 117 && (unsigned char)tb_s548[2] == 110 && (unsigned char)tb_s548[3] == 99 && tb_s548[4] == 0); })) {
                f_check__resolve_params(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1483))->q_kids, 0LL, sizeof(tb_o_Node *), 1483)));
                f_check__resolve_params(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1484))->q_kids, 2LL, sizeof(tb_o_Node *), 1484)));
                tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1485))->q_kids, 2LL, sizeof(tb_o_Node *), 1485));
                if (((((tb_o_Node *)tb_nn(v_outputs, 1486))->q_kids)->len == 1LL)) {
                    { long long tb_v549 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1487))->q_kids, 0LL, sizeof(tb_o_Node *), 1487)), 1487))->q_ty; long long *tb_p549 = &(((tb_o_Node *)tb_nn(v_top, 1487))->q_ty); *tb_p549 = tb_v549; }
                } else if (((((tb_o_Node *)tb_nn(v_outputs, 1488))->q_kids)->len > 1LL)) {
                    { long long tb_v550 = f_check__multi_type(v_c, v_top); long long *tb_p550 = &(((tb_o_Node *)tb_nn(v_top, 1489))->q_ty); *tb_p550 = tb_v550; }
                } else if ((!f_check__has_value_return((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1490))->q_kids, 1LL, sizeof(tb_o_Node *), 1490))))) {
                    { long long tb_v551 = 0LL; long long *tb_p551 = &(((tb_o_Node *)tb_nn(v_top, 1492))->q_ty); *tb_p551 = tb_v551; }
                }
            }
        }
    }
    long long v_main_func = f_check__find_func_in(v_c, "main", v_main_mod);
    if (((((tb_o_Node *)tb_nn(v_program, 1494))->q_kids)->len > 0LL)) {
        { const char * tb_v552 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1495))->q_kids, 0LL, sizeof(tb_o_Node *), 1495)), 1495))->q_file; const char * *tb_p552 = &(((tb_o_Comp *)tb_nn(v_c, 1495))->q_file_name); *tb_p552 = tb_v552; }
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
            long long tb_end553 = ((((tb_o_Comp *)tb_nn(v_c, 1507))->q_funcs)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end553; v_i++) {
                if (((((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1508))->q_funcs, v_i, sizeof(tb_o_Func *), 1508)), 1508))->q_state != 0LL) || (!f_check__links_known(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1508))->q_funcs, v_i, sizeof(tb_o_Func *), 1508)), 1508))->q_node)))) {
                    continue;
                }
                f_check__cut_vars(v_c, 0LL);
                { long long tb_v554 = 0LL; long long *tb_p554 = &(((tb_o_Comp *)tb_nn(v_c, 1511))->q_depth); *tb_p554 = tb_v554; }
                { long long tb_v555 = 0LL; long long *tb_p555 = &(((tb_o_Comp *)tb_nn(v_c, 1512))->q_var_base); *tb_p555 = tb_v555; }
                f_check__check_func(v_c, v_i);
                v_progress = 1;
            }
        }
    }
    {
        tb_list * tb_l556 = ((tb_o_Comp *)tb_nn(v_c, 1515))->q_funcs;
        for (long long tb_i556 = 0; tb_i556 < tb_l556->len; tb_i556++) {
            tb_o_Func * const v_f = ((tb_o_Func * *)tb_l556->data)[tb_i556];
            if ((((tb_o_Func *)tb_nn(v_f, 1516))->q_state == 0LL)) {
                { const char * tb_v557 = ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1517))->q_node, 1517))->q_file; const char * *tb_p557 = &(((tb_o_Comp *)tb_nn(v_c, 1517))->q_file_name); *tb_p557 = tb_v557; }
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1518))->q_node, 1518))->q_line, "функция с link ни разу не вызвана, поэтому тип ярлыка неизвестен", ((tb_o_Func *)tb_nn(v_f, 1518))->q_name);
            }
        }
    }
    f_check__check_piece_leaks(v_c);
    f_check__mark_hot_fields(v_c);
}

static void f_emit__put(tb_o_Comp * v_c, const char * v_s) {
    { const char * tb_v558 = v_s; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 10))->q_out) = tb_v558; }
}

static const char * f_emit__fname(tb_o_Node * v_f) {
    long long tb_rl_mod = -1, tb_rc_mod = 0; (void)tb_rl_mod; (void)tb_rc_mod;
    const char * v_mod = ""; tb_rl_mod = -1;
    {
        const char * tb_l559 = ((tb_o_Node *)tb_nn(v_f, 15))->q_mod;
        for (long long tb_i559 = 0; tb_l559[tb_i559] != 0; tb_i559++) {
            const char * const v_ch = tb_chr((unsigned char)tb_l559[tb_i559]);
            long long v_k = tb_code(v_ch, 16);
            if (((((v_k >= 97LL) && (v_k <= 122LL)) || ((v_k >= 65LL) && (v_k <= 90LL))) || ((v_k >= 48LL) && (v_k <= 57LL)))) {
                tb_app(&v_mod, &tb_rl_mod, &tb_rc_mod, v_ch); 
            } else {
                tb_app_n(&v_mod, &tb_rl_mod, &tb_rc_mod, "_", sizeof("_") - 1); 
            }
        }
    }
    return tb_cat(tb_cat(tb_cat("f_", v_mod), "__"), ((tb_o_Node *)tb_nn(v_f, 21))->q_name);
    tb_crash(13, "функция fname дошла до конца без return");
    return "";
}

static const char * f_emit__c_type(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    if (({ const char *tb_s560 = v_k; ((unsigned char)tb_s560[0] == 105 && (unsigned char)tb_s560[1] == 110 && (unsigned char)tb_s560[2] == 116 && tb_s560[3] == 0); })) {
        return "long long";
    }
    if (({ const char *tb_s561 = v_k; ((unsigned char)tb_s561[0] == 100 && (unsigned char)tb_s561[1] == 101 && (unsigned char)tb_s561[2] == 99 && tb_s561[3] == 0); })) {
        return "double";
    }
    if (({ const char *tb_s562 = v_k; ((unsigned char)tb_s562[0] == 98 && (unsigned char)tb_s562[1] == 111 && (unsigned char)tb_s562[2] == 111 && (unsigned char)tb_s562[3] == 108 && tb_s562[4] == 0); })) {
        return "_Bool";
    }
    if (({ const char *tb_s563 = v_k; ((unsigned char)tb_s563[0] == 116 && (unsigned char)tb_s563[1] == 101 && (unsigned char)tb_s563[2] == 120 && (unsigned char)tb_s563[3] == 116 && tb_s563[4] == 0); })) {
        return "const char *";
    }
    if (({ const char *tb_s564 = v_k; ((unsigned char)tb_s564[0] == 108 && (unsigned char)tb_s564[1] == 105 && (unsigned char)tb_s564[2] == 115 && (unsigned char)tb_s564[3] == 116 && tb_s564[4] == 0); })) {
        if (f_check__is_cols_list(v_c, v_t)) {
            return tb_cat(tb_cat("tb_cols_", ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 35))->q_types, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 35))->q_types, v_t, sizeof(tb_o_Type *), 35)), 35))->q_elem, sizeof(tb_o_Type *), 35)), 35))->q_obj_node, 35))->q_name), " *");
        }
        return "tb_list *";
    }
    if (({ const char *tb_s565 = v_k; ((unsigned char)tb_s565[0] == 100 && (unsigned char)tb_s565[1] == 105 && (unsigned char)tb_s565[2] == 99 && (unsigned char)tb_s565[3] == 116 && tb_s565[4] == 0); })) {
        return "tb_dict *";
    }
    if (({ const char *tb_s566 = v_k; ((unsigned char)tb_s566[0] == 111 && (unsigned char)tb_s566[1] == 98 && (unsigned char)tb_s566[2] == 106 && tb_s566[3] == 0); })) {
        if (f_check__is_cols_obj(v_c, v_t)) {
            return tb_cat("tb_row_", ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 41))->q_types, v_t, sizeof(tb_o_Type *), 41)), 41))->q_obj_node, 41))->q_name);
        }
        return tb_cat(tb_cat("tb_o_", ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 42))->q_types, v_t, sizeof(tb_o_Type *), 42)), 42))->q_obj_node, 42))->q_name), " *");
    }
    if (({ const char *tb_s567 = v_k; ((unsigned char)tb_s567[0] == 109 && (unsigned char)tb_s567[1] == 117 && (unsigned char)tb_s567[2] == 108 && (unsigned char)tb_s567[3] == 116 && (unsigned char)tb_s567[4] == 105 && tb_s567[5] == 0); })) {
        return tb_cat("tb_r_", f_emit__fname(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 44))->q_types, v_t, sizeof(tb_o_Type *), 44)), 44))->q_func_node));
    }
    if (({ const char *tb_s568 = v_k; ((unsigned char)tb_s568[0] == 116 && (unsigned char)tb_s568[1] == 97 && (unsigned char)tb_s568[2] == 115 && (unsigned char)tb_s568[3] == 107 && tb_s568[4] == 0); })) {
        return "tb_task *";
    }
    return "void";
    tb_crash(23, "функция c_type дошла до конца без return");
    return "";
}

static void f_emit__indent(tb_o_Comp * v_c, long long v_depth) {
    {
        long long tb_end569 = v_depth;
        for (long long v_i = 1LL; v_i <= tb_end569; v_i++) {
            f_emit__put(v_c, "    ");
        }
    }
}

static _Bool f_emit__is_bits(tb_o_Comp * v_c, long long v_t) {
    return (({ const char *tb_s570 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s570[0] == 108 && (unsigned char)tb_s570[1] == 105 && (unsigned char)tb_s570[2] == 115 && (unsigned char)tb_s570[3] == 116 && tb_s570[4] == 0); }) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 55))->q_types, v_t, sizeof(tb_o_Type *), 55)), 55))->q_elem == 3LL));
    tb_crash(54, "функция is_bits дошла до конца без return");
    return 0;
}

static void f_emit__emit_default(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    if (({ const char *tb_s571 = v_k; ((unsigned char)tb_s571[0] == 105 && (unsigned char)tb_s571[1] == 110 && (unsigned char)tb_s571[2] == 116 && tb_s571[3] == 0); })) {
        f_emit__put(v_c, "0LL");
    } else if (({ const char *tb_s572 = v_k; ((unsigned char)tb_s572[0] == 100 && (unsigned char)tb_s572[1] == 101 && (unsigned char)tb_s572[2] == 99 && tb_s572[3] == 0); })) {
        f_emit__put(v_c, "0.0");
    } else if (({ const char *tb_s573 = v_k; ((unsigned char)tb_s573[0] == 116 && (unsigned char)tb_s573[1] == 101 && (unsigned char)tb_s573[2] == 120 && (unsigned char)tb_s573[3] == 116 && tb_s573[4] == 0); })) {
        f_emit__put(v_c, "\"\"");
    } else if ((({ const char *tb_s574 = v_k; ((unsigned char)tb_s574[0] == 108 && (unsigned char)tb_s574[1] == 105 && (unsigned char)tb_s574[2] == 115 && (unsigned char)tb_s574[3] == 116 && tb_s574[4] == 0); }) && f_check__is_cols_list(v_c, v_t))) {
        f_emit__put(v_c, tb_cat(tb_cat("tb_cols_new_", f_emit__cols_name(v_c, v_t)), "()"));
    } else if ((({ const char *tb_s575 = v_k; ((unsigned char)tb_s575[0] == 111 && (unsigned char)tb_s575[1] == 98 && (unsigned char)tb_s575[2] == 106 && tb_s575[3] == 0); }) && f_check__is_cols_obj(v_c, v_t))) {
        f_emit__put(v_c, tb_cat(tb_cat("(", f_emit__c_type(v_c, v_t)), "){0, 0}"));
    } else if ((({ const char *tb_s576 = v_k; ((unsigned char)tb_s576[0] == 108 && (unsigned char)tb_s576[1] == 105 && (unsigned char)tb_s576[2] == 115 && (unsigned char)tb_s576[3] == 116 && tb_s576[4] == 0); }) && f_emit__is_bits(v_c, v_t))) {
        f_emit__put(v_c, "tb_bits_new()");
    } else if (({ const char *tb_s577 = v_k; ((unsigned char)tb_s577[0] == 108 && (unsigned char)tb_s577[1] == 105 && (unsigned char)tb_s577[2] == 115 && (unsigned char)tb_s577[3] == 116 && tb_s577[4] == 0); })) {
        f_emit__put(v_c, tb_cat(tb_cat("tb_list_new(sizeof(", f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 72))->q_types, v_t, sizeof(tb_o_Type *), 72)), 72))->q_elem)), "))"));
    } else if (({ const char *tb_s578 = v_k; ((unsigned char)tb_s578[0] == 100 && (unsigned char)tb_s578[1] == 105 && (unsigned char)tb_s578[2] == 99 && (unsigned char)tb_s578[3] == 116 && tb_s578[4] == 0); })) {
        const char * v_is_text = "0";
        if (({ const char *tb_s579 = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 75))->q_types, v_t, sizeof(tb_o_Type *), 75)), 75))->q_key); ((unsigned char)tb_s579[0] == 116 && (unsigned char)tb_s579[1] == 101 && (unsigned char)tb_s579[2] == 120 && (unsigned char)tb_s579[3] == 116 && tb_s579[4] == 0); })) {
            v_is_text = "1";
        }
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("tb_dict_new(", v_is_text), ", sizeof("), f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 77))->q_types, v_t, sizeof(tb_o_Type *), 77)), 77))->q_elem)), "))"));
    } else {
        f_emit__put(v_c, "0");
    }
}

static const char * f_emit__cols_name(tb_o_Comp * v_c, long long v_t) {
    if (({ const char *tb_s580 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s580[0] == 108 && (unsigned char)tb_s580[1] == 105 && (unsigned char)tb_s580[2] == 115 && (unsigned char)tb_s580[3] == 116 && tb_s580[4] == 0); })) {
        return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 84))->q_types, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 84))->q_types, v_t, sizeof(tb_o_Type *), 84)), 84))->q_elem, sizeof(tb_o_Type *), 84)), 84))->q_obj_node, 84))->q_name;
    }
    return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 85))->q_types, v_t, sizeof(tb_o_Type *), 85)), 85))->q_obj_node, 85))->q_name;
    tb_crash(82, "функция cols_name дошла до конца без return");
    return "";
}

static _Bool f_emit__is_simple(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    return (((({ const char *tb_s581 = v_k; ((unsigned char)tb_s581[0] == 105 && (unsigned char)tb_s581[1] == 110 && (unsigned char)tb_s581[2] == 116 && tb_s581[3] == 0); }) || ({ const char *tb_s582 = v_k; ((unsigned char)tb_s582[0] == 100 && (unsigned char)tb_s582[1] == 101 && (unsigned char)tb_s582[2] == 99 && tb_s582[3] == 0); })) || ({ const char *tb_s583 = v_k; ((unsigned char)tb_s583[0] == 98 && (unsigned char)tb_s583[1] == 111 && (unsigned char)tb_s583[2] == 111 && (unsigned char)tb_s583[3] == 108 && tb_s583[4] == 0); })) || ({ const char *tb_s584 = v_k; ((unsigned char)tb_s584[0] == 116 && (unsigned char)tb_s584[1] == 101 && (unsigned char)tb_s584[2] == 120 && (unsigned char)tb_s584[3] == 116 && tb_s584[4] == 0); }));
    tb_crash(87, "функция is_simple дошла до конца без return");
    return 0;
}

static void f_emit__emit_key(tb_o_Comp * v_c, long long v_dt, tb_o_Node * v_key) {
    f_emit__put(v_c, tb_cat(tb_cat("&(", f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 93))->q_types, v_dt, sizeof(tb_o_Type *), 93)), 93))->q_key)), "){"));
    f_emit__emit_expr(v_c, v_key);
    f_emit__put(v_c, "}");
}

static void f_emit__emit_as(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_want) {
    if ((({ const char *tb_s585 = f_check__kind_of(v_c, v_want); ((unsigned char)tb_s585[0] == 100 && (unsigned char)tb_s585[1] == 101 && (unsigned char)tb_s585[2] == 99 && tb_s585[3] == 0); }) && ({ const char *tb_s586 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 101))->q_ty); ((unsigned char)tb_s586[0] == 105 && (unsigned char)tb_s586[1] == 110 && (unsigned char)tb_s586[2] == 116 && tb_s586[3] == 0); }))) {
        f_emit__put(v_c, "(double)(");
        f_emit__emit_expr(v_c, v_n);
        f_emit__put(v_c, ")");
        return;
    }
    f_emit__emit_expr(v_c, v_n);
}

static void f_emit__emit_as_text(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 110))->q_ty);
    if (({ const char *tb_s587 = v_k; ((unsigned char)tb_s587[0] == 116 && (unsigned char)tb_s587[1] == 101 && (unsigned char)tb_s587[2] == 120 && (unsigned char)tb_s587[3] == 116 && tb_s587[4] == 0); })) {
        f_emit__emit_q(v_c, v_n);
        return;
    }
    if (({ const char *tb_s588 = v_k; ((unsigned char)tb_s588[0] == 105 && (unsigned char)tb_s588[1] == 110 && (unsigned char)tb_s588[2] == 116 && tb_s588[3] == 0); })) {
        f_emit__put(v_c, "tb_itos(");
    } else if (({ const char *tb_s589 = v_k; ((unsigned char)tb_s589[0] == 100 && (unsigned char)tb_s589[1] == 101 && (unsigned char)tb_s589[2] == 99 && tb_s589[3] == 0); })) {
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
        long long tb_end590 = ((((tb_o_Node *)tb_nn(v_n, 125))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end590; v_i++) {
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 126))->q_kids, v_i, sizeof(tb_o_Node *), 126)), 126))->q_kids, 0LL, sizeof(tb_o_Node *), 126));
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", tb_ps(\" \"), ");
            }
            f_emit__put(v_c, tb_cat(tb_cat("tb_show_", tb_itos(((tb_o_Node *)tb_nn(v_value, 129))->q_ty)), "("));
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
        long long tb_end591 = ((((tb_o_Node *)tb_nn(v_n, 138))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end591; v_i++) {
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
        f_emit__put(v_c, tb_cat("", tb_itos(((tb_o_Node *)tb_nn(v_n, 145))->q_line)));
    }
    f_emit__put(v_c, ")");
}

static void f_emit__emit_builtin(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_name = ((tb_o_Node *)tb_nn(v_n, 149))->q_name;
    if (({ const char *tb_s592 = v_name; ((unsigned char)tb_s592[0] == 112 && (unsigned char)tb_s592[1] == 114 && (unsigned char)tb_s592[2] == 105 && (unsigned char)tb_s592[3] == 110 && (unsigned char)tb_s592[4] == 116 && tb_s592[5] == 0); })) {
        f_emit__emit_print(v_c, v_n);
        return;
    }
    if (({ const char *tb_s593 = v_name; ((unsigned char)tb_s593[0] == 97 && (unsigned char)tb_s593[1] == 114 && (unsigned char)tb_s593[2] == 103 && (unsigned char)tb_s593[3] == 115 && tb_s593[4] == 0); })) {
        f_emit__put(v_c, "tb_args");
        return;
    }
    if (({ const char *tb_s594 = v_name; ((unsigned char)tb_s594[0] == 105 && (unsigned char)tb_s594[1] == 110 && (unsigned char)tb_s594[2] == 112 && (unsigned char)tb_s594[3] == 117 && (unsigned char)tb_s594[4] == 116 && tb_s594[5] == 0); })) {
        f_emit__put(v_c, "tb_input()");
        return;
    }
    tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 159))->q_kids, 0LL, sizeof(tb_o_Node *), 159)), 159))->q_kids, 0LL, sizeof(tb_o_Node *), 159));
    if (({ const char *tb_s595 = v_name; ((unsigned char)tb_s595[0] == 108 && (unsigned char)tb_s595[1] == 101 && (unsigned char)tb_s595[2] == 110 && tb_s595[3] == 0); })) {
        const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 161))->q_ty);
        if ((({ const char *tb_s596 = v_k; ((unsigned char)tb_s596[0] == 116 && (unsigned char)tb_s596[1] == 101 && (unsigned char)tb_s596[2] == 120 && (unsigned char)tb_s596[3] == 116 && tb_s596[4] == 0); }) && f_emit__is_bld(v_c, v_a))) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("(tb_rl_", ((tb_o_Node *)tb_nn(v_a, 163))->q_name), " >= 0 ? tb_rl_"), ((tb_o_Node *)tb_nn(v_a, 163))->q_name), " : tb_tlen(v_"), ((tb_o_Node *)tb_nn(v_a, 163))->q_name), "))"));
        } else if (({ const char *tb_s597 = v_k; ((unsigned char)tb_s597[0] == 116 && (unsigned char)tb_s597[1] == 101 && (unsigned char)tb_s597[2] == 120 && (unsigned char)tb_s597[3] == 116 && tb_s597[4] == 0); })) {
            f_emit__put(v_c, "tb_tlen(");
            f_emit__emit_expr(v_c, v_a);
            f_emit__put(v_c, ")");
        } else if (({ const char *tb_s598 = v_k; ((unsigned char)tb_s598[0] == 100 && (unsigned char)tb_s598[1] == 105 && (unsigned char)tb_s598[2] == 99 && (unsigned char)tb_s598[3] == 116 && tb_s598[4] == 0); })) {
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
    if (({ const char *tb_s599 = v_name; ((unsigned char)tb_s599[0] == 115 && (unsigned char)tb_s599[1] == 111 && (unsigned char)tb_s599[2] == 114 && (unsigned char)tb_s599[3] == 116 && tb_s599[4] == 0); })) {
        const char * v_k = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 178))->q_types, ((tb_o_Node *)tb_nn(v_a, 178))->q_ty, sizeof(tb_o_Type *), 178)), 178))->q_elem);
        f_emit__put(v_c, "tb_sort_list(");
        f_emit__emit_expr(v_c, v_a);
        if (({ const char *tb_s600 = v_k; ((unsigned char)tb_s600[0] == 105 && (unsigned char)tb_s600[1] == 110 && (unsigned char)tb_s600[2] == 116 && tb_s600[3] == 0); })) {
            f_emit__put(v_c, ", tb_cmp_int)");
        } else if (({ const char *tb_s601 = v_k; ((unsigned char)tb_s601[0] == 100 && (unsigned char)tb_s601[1] == 101 && (unsigned char)tb_s601[2] == 99 && tb_s601[3] == 0); })) {
            f_emit__put(v_c, ", tb_cmp_dec)");
        } else {
            f_emit__put(v_c, ", tb_cmp_text)");
        }
        return;
    }
    if (({ const char *tb_s602 = v_name; ((unsigned char)tb_s602[0] == 97 && (unsigned char)tb_s602[1] == 98 && (unsigned char)tb_s602[2] == 115 && tb_s602[3] == 0); })) {
        if (({ const char *tb_s603 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 189))->q_ty); ((unsigned char)tb_s603[0] == 105 && (unsigned char)tb_s603[1] == 110 && (unsigned char)tb_s603[2] == 116 && tb_s603[3] == 0); })) {
            f_emit__put(v_c, "tb_absi(");
        } else {
            f_emit__put(v_c, "fabs(");
        }
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ")");
        return;
    }
    if (({ const char *tb_s604 = v_name; ((unsigned char)tb_s604[0] == 115 && (unsigned char)tb_s604[1] == 113 && (unsigned char)tb_s604[2] == 114 && (unsigned char)tb_s604[3] == 116 && tb_s604[4] == 0); })) {
        f_emit__put(v_c, "tb_sqrt((double)(");
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, tb_cat(tb_cat("), ", tb_itos(((tb_o_Node *)tb_nn(v_n, 199))->q_line)), ")"));
        return;
    }
    if ((({ const char *tb_s605 = v_name; ((unsigned char)tb_s605[0] == 109 && (unsigned char)tb_s605[1] == 105 && (unsigned char)tb_s605[2] == 110 && tb_s605[3] == 0); }) || ({ const char *tb_s606 = v_name; ((unsigned char)tb_s606[0] == 109 && (unsigned char)tb_s606[1] == 97 && (unsigned char)tb_s606[2] == 120 && tb_s606[3] == 0); }))) {
        _Bool v_ints = ({ const char *tb_s607 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 202))->q_ty); ((unsigned char)tb_s607[0] == 105 && (unsigned char)tb_s607[1] == 110 && (unsigned char)tb_s607[2] == 116 && tb_s607[3] == 0); });
        if (v_ints) {
            f_emit__put(v_c, tb_cat(tb_cat("tb_", v_name), "i("));
            f_emit__emit_expr(v_c, v_a);
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 207))->q_kids, 1LL, sizeof(tb_o_Node *), 207)), 207))->q_kids, 0LL, sizeof(tb_o_Node *), 207)));
        } else {
            f_emit__put(v_c, tb_cat(tb_cat("tb_", v_name), "d("));
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
            f_emit__put(v_c, tb_cat((*(const char * *)tb_at(v_c_names, (v_i + 1LL), sizeof(const char *), 220)), "("));
            f_emit__emit_args(v_c, v_n, ({ const char *tb_s608 = (*(const char * *)tb_at(v_c_names, (v_i + 2LL), sizeof(const char *), 221)); ((unsigned char)tb_s608[0] == 49 && tb_s608[1] == 0); }));
            return;
        }
        { long long tb_v609 = 3LL; long long *tb_p609 = &(v_i); *tb_p609 += tb_v609; }
    }
    f_emit__put(v_c, tb_cat(tb_cat("/* неизвестная встроенная ", v_name), " */0"));
}

static void f_emit__emit_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if ((!((tb_o_Node *)tb_nn(v_n, 227))->q_has_ref)) {
        f_emit__emit_builtin(v_c, v_n);
        return;
    }
    if (({ const char *tb_s610 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 230))->q_ref, 230))->q_kind; ((unsigned char)tb_s610[0] == 111 && (unsigned char)tb_s610[1] == 98 && (unsigned char)tb_s610[2] == 106 && tb_s610[3] == 0); })) {
        tb_o_Node * v_o = ((tb_o_Node *)tb_nn(v_n, 231))->q_ref;
        f_emit__put(v_c, tb_cat(tb_cat("tb_new_", ((tb_o_Node *)tb_nn(v_o, 232))->q_name), "("));
        {
            long long tb_end611 = ((((tb_o_Node *)tb_nn(v_o, 233))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end611; v_i++) {
                if ((v_i > 0LL)) {
                    f_emit__put(v_c, ", ");
                }
                if (({ const char *tb_s612 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 236))->q_kids, v_i, sizeof(tb_o_Node *), 236)), 236))->q_kind; ((unsigned char)tb_s612[0] == 109 && (unsigned char)tb_s612[1] == 105 && (unsigned char)tb_s612[2] == 115 && (unsigned char)tb_s612[3] == 115 && (unsigned char)tb_s612[4] == 105 && (unsigned char)tb_s612[5] == 110 && (unsigned char)tb_s612[6] == 103 && tb_s612[7] == 0); })) {
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
    f_emit__put(v_c, tb_cat(f_emit__fname(((tb_o_Node *)tb_nn(v_n, 243))->q_ref), "("));
    {
        long long tb_end613 = ((((tb_o_Node *)tb_nn(v_n, 244))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end613; v_i++) {
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
    if (({ const char *tb_s614 = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn(v_t, 260))->q_key); ((unsigned char)tb_s614[0] == 116 && (unsigned char)tb_s614[1] == 101 && (unsigned char)tb_s614[2] == 120 && (unsigned char)tb_s614[3] == 116 && tb_s614[4] == 0); })) {
        v_is_text = "1";
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("tb_dict_from(", v_is_text), ", sizeof("), v_vt), "), "), tb_itos(tb_div((((tb_o_Node *)tb_nn(v_n, 262))->q_kids)->len, 2LL, 262))), ", ("), v_kt), "[]){"));
    long long v_i = 0LL;
    while ((v_i < (((tb_o_Node *)tb_nn(v_n, 264))->q_kids)->len)) {
        if ((v_i > 0LL)) {
            f_emit__put(v_c, ", ");
        }
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 267))->q_kids, v_i, sizeof(tb_o_Node *), 267)));
        { long long tb_v615 = 2LL; long long *tb_p615 = &(v_i); *tb_p615 += tb_v615; }
    }
    f_emit__put(v_c, tb_cat(tb_cat("}, (", v_vt), "[]){"));
    v_i = 1LL;
    while ((v_i < (((tb_o_Node *)tb_nn(v_n, 271))->q_kids)->len)) {
        if ((v_i > 1LL)) {
            f_emit__put(v_c, ", ");
        }
        f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 274))->q_kids, v_i, sizeof(tb_o_Node *), 274)), ((tb_o_Type *)tb_nn(v_t, 274))->q_elem);
        { long long tb_v616 = 2LL; long long *tb_p616 = &(v_i); *tb_p616 += tb_v616; }
    }
    f_emit__put(v_c, "})");
}

static void f_emit__emit_method(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_base = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 280))->q_kids, 0LL, sizeof(tb_o_Node *), 280));
    if (({ const char *tb_s617 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_base, 281))->q_ty); ((unsigned char)tb_s617[0] == 100 && (unsigned char)tb_s617[1] == 105 && (unsigned char)tb_s617[2] == 99 && (unsigned char)tb_s617[3] == 116 && tb_s617[4] == 0); })) {
        if ((({ const char *tb_s618 = ((tb_o_Node *)tb_nn(v_n, 282))->q_name; ((unsigned char)tb_s618[0] == 118 && (unsigned char)tb_s618[1] == 97 && (unsigned char)tb_s618[2] == 108 && (unsigned char)tb_s618[3] == 117 && (unsigned char)tb_s618[4] == 101 && (unsigned char)tb_s618[5] == 115 && tb_s618[6] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_n, 282))->q_ty))) {
            f_emit__put(v_c, tb_cat(tb_cat("tb_colsfrom_", f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn(v_n, 283))->q_ty)), "(("));
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, tb_cat(tb_cat(")->vals->len, (", f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 285))->q_types, ((tb_o_Node *)tb_nn(v_n, 285))->q_ty, sizeof(tb_o_Type *), 285)), 285))->q_elem)), " *)("));
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, ")->vals->data)");
            return;
        }
        if ((({ const char *tb_s619 = ((tb_o_Node *)tb_nn(v_n, 289))->q_name; ((unsigned char)tb_s619[0] == 118 && (unsigned char)tb_s619[1] == 97 && (unsigned char)tb_s619[2] == 108 && (unsigned char)tb_s619[3] == 117 && (unsigned char)tb_s619[4] == 101 && (unsigned char)tb_s619[5] == 115 && tb_s619[6] == 0); }) && f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_n, 289))->q_ty))) {
            f_emit__put(v_c, "tb_bits_of_bytes((");
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, ")->vals)");
            return;
        }
        if ((({ const char *tb_s620 = ((tb_o_Node *)tb_nn(v_n, 294))->q_name; ((unsigned char)tb_s620[0] == 107 && (unsigned char)tb_s620[1] == 101 && (unsigned char)tb_s620[2] == 121 && (unsigned char)tb_s620[3] == 115 && tb_s620[4] == 0); }) || ({ const char *tb_s621 = ((tb_o_Node *)tb_nn(v_n, 294))->q_name; ((unsigned char)tb_s621[0] == 118 && (unsigned char)tb_s621[1] == 97 && (unsigned char)tb_s621[2] == 108 && (unsigned char)tb_s621[3] == 117 && (unsigned char)tb_s621[4] == 101 && (unsigned char)tb_s621[5] == 115 && tb_s621[6] == 0); }))) {
            f_emit__put(v_c, "tb_copy((");
            f_emit__emit_expr(v_c, v_base);
            if (({ const char *tb_s622 = ((tb_o_Node *)tb_nn(v_n, 297))->q_name; ((unsigned char)tb_s622[0] == 107 && (unsigned char)tb_s622[1] == 101 && (unsigned char)tb_s622[2] == 121 && (unsigned char)tb_s622[3] == 115 && tb_s622[4] == 0); })) {
                f_emit__put(v_c, ")->keys)");
            } else {
                f_emit__put(v_c, ")->vals)");
            }
            return;
        }
        if (({ const char *tb_s623 = ((tb_o_Node *)tb_nn(v_n, 302))->q_name; ((unsigned char)tb_s623[0] == 104 && (unsigned char)tb_s623[1] == 97 && (unsigned char)tb_s623[2] == 115 && tb_s623[3] == 0); })) {
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
        f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 314))->q_line)), ")"));
        return;
    }
    f_emit__put(v_c, tb_cat(tb_cat("(*(", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 316))->q_ty)), " *)tb_pop("));
    f_emit__emit_expr(v_c, v_base);
    f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 318))->q_line)), "))"));
}

static void f_emit__emit_list(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 321))->q_types, ((tb_o_Node *)tb_nn(v_n, 321))->q_ty, sizeof(tb_o_Type *), 321)), 321))->q_elem;
    const char * v_elem = f_emit__c_type(v_c, v_elem_ty);
    if (f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_n, 323))->q_ty)) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("tb_colsfrom_", f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn(v_n, 324))->q_ty)), "("), tb_itos((((tb_o_Node *)tb_nn(v_n, 324))->q_kids)->len)), ", ("), v_elem), "[]){"));
    } else if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_n, 325))->q_ty)) {
        f_emit__put(v_c, tb_cat(tb_cat("tb_bits_from(", tb_itos((((tb_o_Node *)tb_nn(v_n, 326))->q_kids)->len)), ", (unsigned char[]){"));
    } else {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("tb_list_from(", tb_itos((((tb_o_Node *)tb_nn(v_n, 328))->q_kids)->len)), ", sizeof("), v_elem), "), ("), v_elem), "[]){"));
    }
    {
        long long tb_end624 = ((((tb_o_Node *)tb_nn(v_n, 329))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end624; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 332))->q_kids, v_i, sizeof(tb_o_Node *), 332)), v_elem_ty);
        }
    }
    f_emit__put(v_c, "})");
}

static void f_emit__emit_binop(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 336))->q_value;
    tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 337))->q_kids, 0LL, sizeof(tb_o_Node *), 337));
    tb_o_Node * v_b = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 338))->q_kids, 1LL, sizeof(tb_o_Node *), 338));
    if (({ const char *tb_s625 = v_op; ((unsigned char)tb_s625[0] == 111 && (unsigned char)tb_s625[1] == 114 && (unsigned char)tb_s625[2] == 101 && (unsigned char)tb_s625[3] == 108 && (unsigned char)tb_s625[4] == 115 && (unsigned char)tb_s625[5] == 101 && tb_s625[6] == 0); })) {
        f_emit__put(v_c, tb_cat(tb_cat("(tb_t", tb_itos(((tb_o_Node *)tb_nn(v_n, 341))->q_temp_no)), " = "));
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ", tb_err ? (tb_err = 0, ");
        f_emit__emit_as(v_c, v_b, ((tb_o_Node *)tb_nn(v_n, 344))->q_ty);
        f_emit__put(v_c, tb_cat(tb_cat(") : tb_t", tb_itos(((tb_o_Node *)tb_nn(v_n, 345))->q_temp_no)), ")"));
        return;
    }
    if (({ const char *tb_s626 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 347))->q_ty); ((unsigned char)tb_s626[0] == 116 && (unsigned char)tb_s626[1] == 101 && (unsigned char)tb_s626[2] == 120 && (unsigned char)tb_s626[3] == 116 && tb_s626[4] == 0); })) {
        f_emit__put(v_c, "tb_cat(");
        f_emit__emit_as_text(v_c, v_a);
        f_emit__put(v_c, ", ");
        f_emit__emit_as_text(v_c, v_b);
        f_emit__put(v_c, ")");
        return;
    }
    if (((({ const char *tb_s627 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 354))->q_ty); ((unsigned char)tb_s627[0] == 116 && (unsigned char)tb_s627[1] == 101 && (unsigned char)tb_s627[2] == 120 && (unsigned char)tb_s627[3] == 116 && tb_s627[4] == 0); }) && (({ const char *tb_s628 = v_op; ((unsigned char)tb_s628[0] == 61 && (unsigned char)tb_s628[1] == 61 && tb_s628[2] == 0); }) || ({ const char *tb_s629 = v_op; ((unsigned char)tb_s629[0] == 33 && (unsigned char)tb_s629[1] == 61 && tb_s629[2] == 0); }))) && (f_emit__short_literal(v_b) || f_emit__short_literal(v_a)))) {
        tb_o_Node * v_lit = v_b;
        tb_o_Node * v_other = v_a;
        if ((!f_emit__short_literal(v_b))) {
            v_lit = v_a;
            v_other = v_b;
        }
        const char * v_body = tb_cut(((tb_o_Node *)tb_nn(v_lit, 361))->q_value, 1LL, (tb_tlen(((tb_o_Node *)tb_nn(v_lit, 361))->q_value) - 2LL), 361);
        { long long tb_v630 = 1LL; long long *tb_p630 = &(((tb_o_Comp *)tb_nn(v_c, 362))->q_label); *tb_p630 += tb_v630; }
        const char * v_sv = tb_cat("tb_s", tb_itos(((tb_o_Comp *)tb_nn(v_c, 363))->q_label));
        f_emit__put(v_c, tb_cat(tb_cat("({ const char *", v_sv), " = "));
        f_emit__emit_q(v_c, v_other);
        f_emit__put(v_c, "; ");
        if (({ const char *tb_s631 = v_op; ((unsigned char)tb_s631[0] == 33 && (unsigned char)tb_s631[1] == 61 && tb_s631[2] == 0); })) {
            f_emit__put(v_c, "!");
        }
        f_emit__put(v_c, "(");
        long long v_i = 0LL;
        {
            const char * tb_l632 = v_body;
            for (long long tb_i632 = 0; tb_l632[tb_i632] != 0; tb_i632++) {
                const char * const v_ch = tb_chr((unsigned char)tb_l632[tb_i632]);
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("(unsigned char)", v_sv), "["), tb_itos(v_i)), "] == "), tb_itos(tb_code(v_ch, 372))), " && "));
                { long long tb_v633 = 1LL; long long *tb_p633 = &(v_i); *tb_p633 += tb_v633; }
            }
        }
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(v_sv, "["), tb_itos(v_i)), "] == 0); })"));
        return;
    }
    if (({ const char *tb_s634 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 376))->q_ty); ((unsigned char)tb_s634[0] == 116 && (unsigned char)tb_s634[1] == 101 && (unsigned char)tb_s634[2] == 120 && (unsigned char)tb_s634[3] == 116 && tb_s634[4] == 0); })) {
        f_emit__put(v_c, "(strcmp(");
        f_emit__emit_q(v_c, v_a);
        f_emit__put(v_c, ", ");
        f_emit__emit_q(v_c, v_b);
        f_emit__put(v_c, tb_cat(tb_cat(") ", v_op), " 0)"));
        return;
    }
    if ((({ const char *tb_s635 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 383))->q_ty); ((unsigned char)tb_s635[0] == 105 && (unsigned char)tb_s635[1] == 110 && (unsigned char)tb_s635[2] == 116 && tb_s635[3] == 0); }) && (({ const char *tb_s636 = v_op; ((unsigned char)tb_s636[0] == 47 && tb_s636[1] == 0); }) || ({ const char *tb_s637 = v_op; ((unsigned char)tb_s637[0] == 37 && tb_s637[1] == 0); })))) {
        if (({ const char *tb_s638 = v_op; ((unsigned char)tb_s638[0] == 47 && tb_s638[1] == 0); })) {
            f_emit__put(v_c, "tb_div(");
        } else {
            f_emit__put(v_c, "tb_mod(");
        }
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, v_b);
        f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 391))->q_line)), ")"));
        return;
    }
    const char * v_c_op = v_op;
    if (({ const char *tb_s639 = v_op; ((unsigned char)tb_s639[0] == 97 && (unsigned char)tb_s639[1] == 110 && (unsigned char)tb_s639[2] == 100 && tb_s639[3] == 0); })) {
        v_c_op = "&&";
    } else if (({ const char *tb_s640 = v_op; ((unsigned char)tb_s640[0] == 111 && (unsigned char)tb_s640[1] == 114 && tb_s640[2] == 0); })) {
        v_c_op = "||";
    }
    f_emit__put(v_c, "(");
    if ((({ const char *tb_s641 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 399))->q_ty); ((unsigned char)tb_s641[0] == 100 && (unsigned char)tb_s641[1] == 101 && (unsigned char)tb_s641[2] == 99 && tb_s641[3] == 0); }) && ({ const char *tb_s642 = v_op; ((unsigned char)tb_s642[0] == 47 && tb_s642[1] == 0); }))) {
        f_emit__emit_as(v_c, v_a, ((tb_o_Node *)tb_nn(v_n, 400))->q_ty);
    } else {
        f_emit__emit_expr(v_c, v_a);
    }
    f_emit__put(v_c, tb_cat(tb_cat(" ", v_c_op), " "));
    f_emit__emit_expr(v_c, v_b);
    f_emit__put(v_c, ")");
}

static void f_emit__emit_expr(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 408))->q_kind;
    if (({ const char *tb_s643 = v_k; ((unsigned char)tb_s643[0] == 110 && (unsigned char)tb_s643[1] == 117 && (unsigned char)tb_s643[2] == 109 && (unsigned char)tb_s643[3] == 98 && (unsigned char)tb_s643[4] == 101 && (unsigned char)tb_s643[5] == 114 && tb_s643[6] == 0); })) {
        f_emit__put(v_c, tb_cat(((tb_o_Node *)tb_nn(v_n, 410))->q_value, "LL"));
    } else if ((({ const char *tb_s644 = v_k; ((unsigned char)tb_s644[0] == 100 && (unsigned char)tb_s644[1] == 101 && (unsigned char)tb_s644[2] == 99 && (unsigned char)tb_s644[3] == 105 && (unsigned char)tb_s644[4] == 109 && (unsigned char)tb_s644[5] == 97 && (unsigned char)tb_s644[6] == 108 && tb_s644[7] == 0); }) || ({ const char *tb_s645 = v_k; ((unsigned char)tb_s645[0] == 115 && (unsigned char)tb_s645[1] == 116 && (unsigned char)tb_s645[2] == 114 && (unsigned char)tb_s645[3] == 105 && (unsigned char)tb_s645[4] == 110 && (unsigned char)tb_s645[5] == 103 && tb_s645[6] == 0); }))) {
        f_emit__put(v_c, ((tb_o_Node *)tb_nn(v_n, 412))->q_value);
    } else if (({ const char *tb_s646 = v_k; ((unsigned char)tb_s646[0] == 98 && (unsigned char)tb_s646[1] == 111 && (unsigned char)tb_s646[2] == 111 && (unsigned char)tb_s646[3] == 108 && tb_s646[4] == 0); })) {
        if (({ const char *tb_s647 = ((tb_o_Node *)tb_nn(v_n, 414))->q_value; ((unsigned char)tb_s647[0] == 116 && (unsigned char)tb_s647[1] == 114 && (unsigned char)tb_s647[2] == 117 && (unsigned char)tb_s647[3] == 101 && tb_s647[4] == 0); })) {
            f_emit__put(v_c, "1");
        } else {
            f_emit__put(v_c, "0");
        }
    } else if (((({ const char *tb_s648 = v_k; ((unsigned char)tb_s648[0] == 110 && (unsigned char)tb_s648[1] == 97 && (unsigned char)tb_s648[2] == 109 && (unsigned char)tb_s648[3] == 101 && tb_s648[4] == 0); }) && ((((tb_o_Comp *)tb_nn(v_c, 418))->q_vec_names)->len > 0LL)) && (f_emit__vec_index(v_c, ((tb_o_Node *)tb_nn(v_n, 418))->q_name) >= 0LL))) {
        f_emit__put(v_c, (*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 419))->q_vec_repl, f_emit__vec_index(v_c, ((tb_o_Node *)tb_nn(v_n, 419))->q_name), sizeof(const char *), 419)));
    } else if (({ const char *tb_s649 = v_k; ((unsigned char)tb_s649[0] == 110 && (unsigned char)tb_s649[1] == 97 && (unsigned char)tb_s649[2] == 109 && (unsigned char)tb_s649[3] == 101 && tb_s649[4] == 0); })) {
        if (((tb_o_Node *)tb_nn(v_n, 421))->q_is_link) {
            f_emit__put(v_c, tb_cat(tb_cat("(*v_", ((tb_o_Node *)tb_nn(v_n, 422))->q_name), ")"));
        } else if (f_emit__is_bld(v_c, v_n)) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("(tb_rl_", ((tb_o_Node *)tb_nn(v_n, 424))->q_name), " = -1, v_"), ((tb_o_Node *)tb_nn(v_n, 424))->q_name), ")"));
        } else {
            f_emit__put(v_c, tb_cat("v_", ((tb_o_Node *)tb_nn(v_n, 426))->q_name));
        }
    } else if (({ const char *tb_s650 = v_k; ((unsigned char)tb_s650[0] == 108 && (unsigned char)tb_s650[1] == 105 && (unsigned char)tb_s650[2] == 115 && (unsigned char)tb_s650[3] == 116 && tb_s650[4] == 0); })) {
        f_emit__emit_list(v_c, v_n);
    } else if (({ const char *tb_s651 = v_k; ((unsigned char)tb_s651[0] == 100 && (unsigned char)tb_s651[1] == 105 && (unsigned char)tb_s651[2] == 99 && (unsigned char)tb_s651[3] == 116 && tb_s651[4] == 0); })) {
        f_emit__emit_dict(v_c, v_n);
    } else if ((({ const char *tb_s652 = v_k; ((unsigned char)tb_s652[0] == 116 && (unsigned char)tb_s652[1] == 114 && (unsigned char)tb_s652[2] == 121 && tb_s652[3] == 0); }) || ({ const char *tb_s653 = v_k; ((unsigned char)tb_s653[0] == 101 && (unsigned char)tb_s653[1] == 108 && (unsigned char)tb_s653[2] == 115 && (unsigned char)tb_s653[3] == 101 && (unsigned char)tb_s653[4] == 95 && (unsigned char)tb_s653[5] == 101 && (unsigned char)tb_s653[6] == 114 && (unsigned char)tb_s653[7] == 114 && tb_s653[8] == 0); }))) {
        f_emit__put(v_c, tb_cat("tb_v", tb_itos(((tb_o_Node *)tb_nn(v_n, 433))->q_temp_no)));
    } else if (({ const char *tb_s654 = v_k; ((unsigned char)tb_s654[0] == 99 && (unsigned char)tb_s654[1] == 97 && (unsigned char)tb_s654[2] == 108 && (unsigned char)tb_s654[3] == 108 && tb_s654[4] == 0); })) {
        f_emit__emit_call(v_c, v_n);
    } else if (({ const char *tb_s655 = v_k; ((unsigned char)tb_s655[0] == 109 && (unsigned char)tb_s655[1] == 101 && (unsigned char)tb_s655[2] == 116 && (unsigned char)tb_s655[3] == 104 && (unsigned char)tb_s655[4] == 111 && (unsigned char)tb_s655[5] == 100 && tb_s655[6] == 0); })) {
        f_emit__emit_method(v_c, v_n);
    } else if (({ const char *tb_s656 = v_k; ((unsigned char)tb_s656[0] == 98 && (unsigned char)tb_s656[1] == 105 && (unsigned char)tb_s656[2] == 110 && (unsigned char)tb_s656[3] == 111 && (unsigned char)tb_s656[4] == 112 && tb_s656[5] == 0); })) {
        f_emit__emit_binop(v_c, v_n);
    } else if (({ const char *tb_s657 = v_k; ((unsigned char)tb_s657[0] == 117 && (unsigned char)tb_s657[1] == 110 && (unsigned char)tb_s657[2] == 97 && (unsigned char)tb_s657[3] == 114 && (unsigned char)tb_s657[4] == 121 && tb_s657[5] == 0); })) {
        if (({ const char *tb_s658 = ((tb_o_Node *)tb_nn(v_n, 441))->q_value; ((unsigned char)tb_s658[0] == 110 && (unsigned char)tb_s658[1] == 111 && (unsigned char)tb_s658[2] == 116 && tb_s658[3] == 0); })) {
            f_emit__put(v_c, "(!");
        } else {
            f_emit__put(v_c, "(-");
        }
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 445))->q_kids, 0LL, sizeof(tb_o_Node *), 445)));
        f_emit__put(v_c, ")");
    } else if ((({ const char *tb_s659 = v_k; ((unsigned char)tb_s659[0] == 105 && (unsigned char)tb_s659[1] == 110 && (unsigned char)tb_s659[2] == 100 && (unsigned char)tb_s659[3] == 101 && (unsigned char)tb_s659[4] == 120 && tb_s659[5] == 0); }) && ({ const char *tb_s660 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 447))->q_kids, 0LL, sizeof(tb_o_Node *), 447)), 447))->q_ty); ((unsigned char)tb_s660[0] == 100 && (unsigned char)tb_s660[1] == 105 && (unsigned char)tb_s660[2] == 99 && (unsigned char)tb_s660[3] == 116 && tb_s660[4] == 0); }))) {
        f_emit__put(v_c, tb_cat(tb_cat("(*(", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 448))->q_ty)), " *)tb_dget("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 449))->q_kids, 0LL, sizeof(tb_o_Node *), 449)));
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 451))->q_kids, 0LL, sizeof(tb_o_Node *), 451)), 451))->q_ty, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 451))->q_kids, 1LL, sizeof(tb_o_Node *), 451)));
        f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 452))->q_line)), "))"));
    } else if ((({ const char *tb_s661 = v_k; ((unsigned char)tb_s661[0] == 105 && (unsigned char)tb_s661[1] == 110 && (unsigned char)tb_s661[2] == 100 && (unsigned char)tb_s661[3] == 101 && (unsigned char)tb_s661[4] == 120 && tb_s661[5] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 453))->q_kids, 0LL, sizeof(tb_o_Node *), 453)), 453))->q_ty))) {
        f_emit__put(v_c, tb_cat(tb_cat("tb_colat_", f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 454))->q_kids, 0LL, sizeof(tb_o_Node *), 454)), 454))->q_ty)), "("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 455))->q_kids, 0LL, sizeof(tb_o_Node *), 455)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 457))->q_kids, 1LL, sizeof(tb_o_Node *), 457)));
        f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 458))->q_line)), ")"));
    } else if (({ const char *tb_s662 = v_k; ((unsigned char)tb_s662[0] == 105 && (unsigned char)tb_s662[1] == 110 && (unsigned char)tb_s662[2] == 100 && (unsigned char)tb_s662[3] == 101 && (unsigned char)tb_s662[4] == 120 && tb_s662[5] == 0); })) {
        if (({ const char *tb_s663 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 460))->q_kids, 0LL, sizeof(tb_o_Node *), 460)), 460))->q_ty); ((unsigned char)tb_s663[0] == 116 && (unsigned char)tb_s663[1] == 101 && (unsigned char)tb_s663[2] == 120 && (unsigned char)tb_s663[3] == 116 && tb_s663[4] == 0); })) {
            f_emit__put(v_c, "tb_char_at(");
            f_emit__emit_q(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 462))->q_kids, 0LL, sizeof(tb_o_Node *), 462)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 464))->q_kids, 1LL, sizeof(tb_o_Node *), 464)));
            if (f_emit__is_bld(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 465))->q_kids, 0LL, sizeof(tb_o_Node *), 465)))) {
                f_emit__put(v_c, tb_cat(", tb_rl_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 466))->q_kids, 0LL, sizeof(tb_o_Node *), 466)), 466))->q_name));
            } else {
                f_emit__put(v_c, ", -1");
            }
            f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 469))->q_line)), ")"));
        } else if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 470))->q_kids, 0LL, sizeof(tb_o_Node *), 470)), 470))->q_ty)) {
            f_emit__put(v_c, "tb_bget(");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 472))->q_kids, 0LL, sizeof(tb_o_Node *), 472)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 474))->q_kids, 1LL, sizeof(tb_o_Node *), 474)));
            f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 475))->q_line)), ")"));
        } else {
            const char * v_t = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 477))->q_ty);
            f_emit__put(v_c, tb_cat(tb_cat("(*(", v_t), " *)tb_at("));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 479))->q_kids, 0LL, sizeof(tb_o_Node *), 479)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 481))->q_kids, 1LL, sizeof(tb_o_Node *), 481)));
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(", sizeof(", v_t), "), "), tb_itos(((tb_o_Node *)tb_nn(v_n, 482))->q_line)), "))"));
        }
    } else if ((({ const char *tb_s664 = v_k; ((unsigned char)tb_s664[0] == 102 && (unsigned char)tb_s664[1] == 105 && (unsigned char)tb_s664[2] == 101 && (unsigned char)tb_s664[3] == 108 && (unsigned char)tb_s664[4] == 100 && tb_s664[5] == 0); }) && ({ const char *tb_s665 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 483))->q_kids, 0LL, sizeof(tb_o_Node *), 483)), 483))->q_ty); ((unsigned char)tb_s665[0] == 116 && (unsigned char)tb_s665[1] == 97 && (unsigned char)tb_s665[2] == 115 && (unsigned char)tb_s665[3] == 107 && tb_s665[4] == 0); }))) {
        long long v_rt = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 484))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 484))->q_kids, 0LL, sizeof(tb_o_Node *), 484)), 484))->q_ty, sizeof(tb_o_Type *), 484)), 484))->q_elem;
        if ((v_rt == 0LL)) {
            f_emit__put(v_c, "((void)tb_wait(");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 487))->q_kids, 0LL, sizeof(tb_o_Node *), 487)));
            f_emit__put(v_c, "))");
        } else {
            f_emit__put(v_c, tb_cat(tb_cat("(*(", f_emit__c_type(v_c, v_rt)), " *)tb_wait("));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 491))->q_kids, 0LL, sizeof(tb_o_Node *), 491)));
            f_emit__put(v_c, "))");
        }
    } else if ((strcmp(v_k, "background") == 0)) {
        f_emit__emit_background(v_c, v_n);
    } else if ((({ const char *tb_s666 = v_k; ((unsigned char)tb_s666[0] == 102 && (unsigned char)tb_s666[1] == 105 && (unsigned char)tb_s666[2] == 101 && (unsigned char)tb_s666[3] == 108 && (unsigned char)tb_s666[4] == 100 && tb_s666[5] == 0); }) && f_check__is_cols_obj(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 495))->q_kids, 0LL, sizeof(tb_o_Node *), 495)), 495))->q_ty))) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("(*tb_colf_", f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 497))->q_kids, 0LL, sizeof(tb_o_Node *), 497)), 497))->q_ty)), "_"), ((tb_o_Node *)tb_nn(v_n, 497))->q_name), "("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 498))->q_kids, 0LL, sizeof(tb_o_Node *), 498)));
        f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 499))->q_line)), "))"));
    } else if (({ const char *tb_s667 = v_k; ((unsigned char)tb_s667[0] == 102 && (unsigned char)tb_s667[1] == 105 && (unsigned char)tb_s667[2] == 101 && (unsigned char)tb_s667[3] == 108 && (unsigned char)tb_s667[4] == 100 && tb_s667[5] == 0); })) {
        f_emit__put(v_c, tb_cat(tb_cat("((", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 501))->q_kids, 0LL, sizeof(tb_o_Node *), 501)), 501))->q_ty)), ")tb_nn("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 502))->q_kids, 0LL, sizeof(tb_o_Node *), 502)));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 503))->q_line)), "))"), f_emit__field_path(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 503))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 503))->q_kids, 0LL, sizeof(tb_o_Node *), 503)), 503))->q_ty, sizeof(tb_o_Type *), 503)), 503))->q_obj_node, ((tb_o_Node *)tb_nn(v_n, 503))->q_name)));
    }
}

static void f_emit__emit_bail(tb_o_Comp * v_c) {
    tb_o_Node * v_f = ((tb_o_Comp *)tb_nn(v_c, 509))->q_cur_func;
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 510))->q_kids, 2LL, sizeof(tb_o_Node *), 510));
    if (({ const char *tb_s668 = ((tb_o_Node *)tb_nn(v_f, 511))->q_name; ((unsigned char)tb_s668[0] == 109 && (unsigned char)tb_s668[1] == 97 && (unsigned char)tb_s668[2] == 105 && (unsigned char)tb_s668[3] == 110 && tb_s668[4] == 0); })) {
        f_emit__put(v_c, "tb_die(tb_err);");
    } else if (((((tb_o_Node *)tb_nn(v_outputs, 513))->q_kids)->len == 1LL)) {
        f_emit__put(v_c, tb_cat(tb_cat("return v_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 514))->q_kids, 0LL, sizeof(tb_o_Node *), 514)), 514))->q_name), ";"));
    } else if (((((tb_o_Node *)tb_nn(v_outputs, 515))->q_kids)->len > 1LL)) {
        f_emit__put(v_c, tb_cat(tb_cat("return (", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 516))->q_ty)), "){"));
        {
            long long tb_end669 = ((((tb_o_Node *)tb_nn(v_outputs, 517))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end669; v_i++) {
                if ((v_i > 0LL)) {
                    f_emit__put(v_c, ", ");
                }
                f_emit__put(v_c, tb_cat("v_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 520))->q_kids, v_i, sizeof(tb_o_Node *), 520)), 520))->q_name));
            }
        }
        f_emit__put(v_c, "};");
    } else if (({ const char *tb_s670 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 522))->q_ty); ((unsigned char)tb_s670[0] == 110 && (unsigned char)tb_s670[1] == 111 && (unsigned char)tb_s670[2] == 110 && (unsigned char)tb_s670[3] == 101 && tb_s670[4] == 0); })) {
        f_emit__put(v_c, "return;");
    } else {
        f_emit__put(v_c, "return ");
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_f, 526))->q_ty);
        f_emit__put(v_c, ";");
    }
}

static void f_emit__emit_pre(tb_o_Comp * v_c, tb_o_Node * v_v, long long v_depth) {
    if ((({ const char *tb_s671 = ((tb_o_Node *)tb_nn(v_v, 531))->q_kind; !((unsigned char)tb_s671[0] == 116 && (unsigned char)tb_s671[1] == 114 && (unsigned char)tb_s671[2] == 121 && tb_s671[3] == 0); }) && ({ const char *tb_s672 = ((tb_o_Node *)tb_nn(v_v, 531))->q_kind; !((unsigned char)tb_s672[0] == 101 && (unsigned char)tb_s672[1] == 108 && (unsigned char)tb_s672[2] == 115 && (unsigned char)tb_s672[3] == 101 && (unsigned char)tb_s672[4] == 95 && (unsigned char)tb_s672[5] == 101 && (unsigned char)tb_s672[6] == 114 && (unsigned char)tb_s672[7] == 114 && tb_s672[8] == 0); }))) {
        return;
    }
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 533))->q_kids, 0LL, sizeof(tb_o_Node *), 533));
    { long long tb_v673 = 1LL; long long *tb_p673 = &(((tb_o_Comp *)tb_nn(v_c, 534))->q_label); *tb_p673 += tb_v673; }
    { long long tb_v674 = ((tb_o_Comp *)tb_nn(v_c, 535))->q_label; long long *tb_p674 = &(((tb_o_Node *)tb_nn(v_v, 535))->q_temp_no); *tb_p674 = tb_v674; }
    f_emit__indent(v_c, v_depth);
    if (({ const char *tb_s675 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_call, 537))->q_ty); !((unsigned char)tb_s675[0] == 110 && (unsigned char)tb_s675[1] == 111 && (unsigned char)tb_s675[2] == 110 && (unsigned char)tb_s675[3] == 101 && tb_s675[4] == 0); })) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_call, 538))->q_ty), " tb_v"), tb_itos(((tb_o_Comp *)tb_nn(v_c, 538))->q_label)), " = "));
    }
    f_emit__emit_expr(v_c, v_call);
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, v_depth);
    if (({ const char *tb_s676 = ((tb_o_Node *)tb_nn(v_v, 542))->q_kind; ((unsigned char)tb_s676[0] == 116 && (unsigned char)tb_s676[1] == 114 && (unsigned char)tb_s676[2] == 121 && tb_s676[3] == 0); })) {
        f_emit__put(v_c, "if (TB_RARE(tb_err != 0)) { ");
        f_emit__emit_bail(v_c);
        f_emit__put(v_c, " }\n");
        return;
    }
    f_emit__put(v_c, "if (tb_err != 0) {\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat("const char * const v_", ((tb_o_Node *)tb_nn(v_v, 549))->q_name), " = tb_err;\n"));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "tb_err = 0;\n");
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 552))->q_kids, 1LL, sizeof(tb_o_Node *), 552)), (v_depth + 1LL));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_target(tb_o_Comp * v_c, tb_o_Node * v_t) {
    if ((({ const char *tb_s677 = ((tb_o_Node *)tb_nn(v_t, 558))->q_kind; ((unsigned char)tb_s677[0] == 105 && (unsigned char)tb_s677[1] == 110 && (unsigned char)tb_s677[2] == 100 && (unsigned char)tb_s677[3] == 101 && (unsigned char)tb_s677[4] == 120 && tb_s677[5] == 0); }) && ({ const char *tb_s678 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 558))->q_kids, 0LL, sizeof(tb_o_Node *), 558)), 558))->q_ty); ((unsigned char)tb_s678[0] == 100 && (unsigned char)tb_s678[1] == 105 && (unsigned char)tb_s678[2] == 99 && (unsigned char)tb_s678[3] == 116 && tb_s678[4] == 0); }))) {
        long long v_d = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 559))->q_kids, 0LL, sizeof(tb_o_Node *), 559)), 559))->q_ty;
        long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 560))->q_types, v_d, sizeof(tb_o_Type *), 560)), 560))->q_elem;
        const char * v_vt = f_emit__c_type(v_c, v_elem);
        f_emit__put(v_c, tb_cat(tb_cat("(*(", v_vt), " *)tb_dput("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 563))->q_kids, 0LL, sizeof(tb_o_Node *), 563)));
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, v_d, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 565))->q_kids, 1LL, sizeof(tb_o_Node *), 565)));
        if (f_emit__is_simple(v_c, v_elem)) {
            f_emit__put(v_c, tb_cat(tb_cat(", &(", v_vt), "){"));
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
    if (((((tb_o_Node *)tb_nn(v_n, 576))->q_kids)->len == 1LL)) {
        f_emit__emit_pre(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 577))->q_kids, 0LL, sizeof(tb_o_Node *), 577)), v_depth);
    }
    f_emit__indent(v_c, v_depth);
    _Bool v_bld = (((!((tb_o_Node *)tb_nn(v_n, 579))->q_is_link) && ({ const char *tb_s679 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 579))->q_ty); ((unsigned char)tb_s679[0] == 116 && (unsigned char)tb_s679[1] == 101 && (unsigned char)tb_s679[2] == 120 && (unsigned char)tb_s679[3] == 116 && tb_s679[4] == 0); })) && f_emit__bld_has(((tb_o_Comp *)tb_nn(v_c, 579))->q_builders, ((tb_o_Node *)tb_nn(v_n, 579))->q_name));
    if ((((v_bld && (!((tb_o_Node *)tb_nn(v_n, 580))->q_creates)) && ((((tb_o_Node *)tb_nn(v_n, 580))->q_kids)->len == 1LL)) && f_emit__app_form(v_c, ((tb_o_Node *)tb_nn(v_n, 580))->q_name, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 580))->q_kids, 0LL, sizeof(tb_o_Node *), 580))))) {
        {
            tb_list * tb_l680 = f_emit__app_parts(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 582))->q_kids, 0LL, sizeof(tb_o_Node *), 582)));
            for (long long tb_i680 = 0; tb_i680 < tb_l680->len; tb_i680++) {
                tb_o_Node * const v_part = ((tb_o_Node * *)tb_l680->data)[tb_i680];
                f_emit__emit_app(v_c, ((tb_o_Node *)tb_nn(v_n, 583))->q_name, v_part);
                f_emit__put(v_c, " ");
            }
        }
        f_emit__put(v_c, "\n");
        return;
    }
    if (((tb_o_Node *)tb_nn(v_n, 587))->q_creates) {
        f_emit__put(v_c, tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 588))->q_ty), " "));
        if (((tb_o_Node *)tb_nn(v_n, 589))->q_is_const) {
            f_emit__put(v_c, "const ");
        }
        f_emit__put(v_c, tb_cat(tb_cat("v_", ((tb_o_Node *)tb_nn(v_n, 591))->q_name), " = "));
    } else if (((tb_o_Node *)tb_nn(v_n, 592))->q_is_link) {
        f_emit__put(v_c, tb_cat(tb_cat("(*v_", ((tb_o_Node *)tb_nn(v_n, 593))->q_name), ") = "));
    } else {
        f_emit__put(v_c, tb_cat(tb_cat("v_", ((tb_o_Node *)tb_nn(v_n, 595))->q_name), " = "));
    }
    if (((((tb_o_Node *)tb_nn(v_n, 596))->q_kids)->len == 1LL)) {
        f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 597))->q_kids, 0LL, sizeof(tb_o_Node *), 597)), ((tb_o_Node *)tb_nn(v_n, 597))->q_ty);
    } else {
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_n, 599))->q_ty);
    }
    f_emit__put(v_c, ";");
    if (v_bld) {
        f_emit__put(v_c, tb_cat(tb_cat(" tb_rl_", ((tb_o_Node *)tb_nn(v_n, 603))->q_name), " = -1;"));
    }
    f_emit__put(v_c, "\n");
}

static void f_emit__emit_set(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 608))->q_kids, 0LL, sizeof(tb_o_Node *), 608));
    tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 609))->q_kids, 1LL, sizeof(tb_o_Node *), 609));
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 610))->q_value;
    const char * v_t = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 611))->q_ty);
    f_emit__emit_pre(v_c, v_value, v_depth);
    { long long tb_v681 = 1LL; long long *tb_p681 = &(((tb_o_Comp *)tb_nn(v_c, 613))->q_label); *tb_p681 += tb_v681; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 614))->q_label));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{ ");
    if ((({ const char *tb_s682 = ((tb_o_Node *)tb_nn(v_target, 617))->q_kind; ((unsigned char)tb_s682[0] == 105 && (unsigned char)tb_s682[1] == 110 && (unsigned char)tb_s682[2] == 100 && (unsigned char)tb_s682[3] == 101 && (unsigned char)tb_s682[4] == 120 && tb_s682[5] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 617))->q_kids, 0LL, sizeof(tb_o_Node *), 617)), 617))->q_ty))) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(v_t, " tb_v"), v_id), " = "));
        f_emit__emit_expr(v_c, v_value);
        f_emit__put(v_c, tb_cat(tb_cat("; tb_colset_", f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 621))->q_kids, 0LL, sizeof(tb_o_Node *), 621)), 621))->q_ty)), "("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 622))->q_kids, 0LL, sizeof(tb_o_Node *), 622)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 624))->q_kids, 1LL, sizeof(tb_o_Node *), 624)));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(", tb_v", v_id), ", "), tb_itos(((tb_o_Node *)tb_nn(v_n, 625))->q_line)), "); }\n"));
        return;
    }
    if (((({ const char *tb_s683 = ((tb_o_Node *)tb_nn(v_target, 627))->q_kind; ((unsigned char)tb_s683[0] == 105 && (unsigned char)tb_s683[1] == 110 && (unsigned char)tb_s683[2] == 100 && (unsigned char)tb_s683[3] == 101 && (unsigned char)tb_s683[4] == 120 && tb_s683[5] == 0); }) && f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 627))->q_kids, 0LL, sizeof(tb_o_Node *), 627)), 627))->q_ty)) && ({ const char *tb_s684 = v_op; ((unsigned char)tb_s684[0] == 61 && tb_s684[1] == 0); }))) {
        f_emit__put(v_c, tb_cat(tb_cat("_Bool tb_v", v_id), " = "));
        f_emit__emit_expr(v_c, v_value);
        f_emit__put(v_c, "; tb_bset(");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 632))->q_kids, 0LL, sizeof(tb_o_Node *), 632)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 634))->q_kids, 1LL, sizeof(tb_o_Node *), 634)));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(", tb_v", v_id), ", "), tb_itos(((tb_o_Node *)tb_nn(v_n, 635))->q_line)), "); }\n"));
        return;
    }
    if ((((({ const char *tb_s685 = v_op; ((unsigned char)tb_s685[0] == 43 && (unsigned char)tb_s685[1] == 61 && tb_s685[2] == 0); }) && f_emit__is_bld(v_c, v_target)) && f_emit__is_text_plus(v_c, v_value)) && (!f_emit__bld_uses(v_value, ((tb_o_Node *)tb_nn(v_target, 637))->q_name)))) {
        f_emit__put(v_c, "} ");
        tb_o_Node * v_r = v_value;
        while (f_emit__is_text_plus(v_c, v_r)) {
            v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 642))->q_kids, 0LL, sizeof(tb_o_Node *), 642));
        }
        f_emit__emit_app(v_c, ((tb_o_Node *)tb_nn(v_target, 643))->q_name, v_r);
        {
            tb_list * tb_l686 = f_emit__app_parts(v_c, v_value);
            for (long long tb_i686 = 0; tb_i686 < tb_l686->len; tb_i686++) {
                tb_o_Node * const v_part = ((tb_o_Node * *)tb_l686->data)[tb_i686];
                f_emit__put(v_c, " ");
                f_emit__emit_app(v_c, ((tb_o_Node *)tb_nn(v_target, 646))->q_name, v_part);
            }
        }
        f_emit__put(v_c, "\n");
        return;
    }
    if ((({ const char *tb_s687 = v_op; ((unsigned char)tb_s687[0] == 43 && (unsigned char)tb_s687[1] == 61 && tb_s687[2] == 0); }) && f_emit__is_bld(v_c, v_target))) {
        f_emit__put(v_c, tb_cat(tb_cat("const char *tb_v", v_id), " = "));
        f_emit__emit_as_text(v_c, v_value);
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("; tb_app(&v_", ((tb_o_Node *)tb_nn(v_target, 653))->q_name), ", &tb_rl_"), ((tb_o_Node *)tb_nn(v_target, 653))->q_name), ", &tb_rc_"), ((tb_o_Node *)tb_nn(v_target, 653))->q_name), ", tb_v"), v_id), "); }\n"));
        return;
    }
    if ((({ const char *tb_s688 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 655))->q_ty); ((unsigned char)tb_s688[0] == 116 && (unsigned char)tb_s688[1] == 101 && (unsigned char)tb_s688[2] == 120 && (unsigned char)tb_s688[3] == 116 && tb_s688[4] == 0); }) && ({ const char *tb_s689 = v_op; !((unsigned char)tb_s689[0] == 61 && tb_s689[1] == 0); }))) {
        f_emit__put(v_c, tb_cat(tb_cat("const char *tb_v", v_id), " = "));
        f_emit__emit_as_text(v_c, v_value);
    } else {
        if (({ const char *tb_s690 = v_op; ((unsigned char)tb_s690[0] == 61 && tb_s690[1] == 0); })) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(v_t, " tb_v"), v_id), " = "));
            f_emit__emit_as(v_c, v_value, ((tb_o_Node *)tb_nn(v_n, 661))->q_ty);
        } else {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_value, 663))->q_ty), " tb_v"), v_id), " = "));
            f_emit__emit_expr(v_c, v_value);
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("; ", v_t), " *tb_p"), v_id), " = &("));
    f_emit__emit_target(v_c, v_target);
    f_emit__put(v_c, "); ");
    if (({ const char *tb_s691 = v_op; ((unsigned char)tb_s691[0] == 61 && tb_s691[1] == 0); })) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("*tb_p", v_id), " = tb_v"), v_id), ";"));
    } else if (({ const char *tb_s692 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 670))->q_ty); ((unsigned char)tb_s692[0] == 116 && (unsigned char)tb_s692[1] == 101 && (unsigned char)tb_s692[2] == 120 && (unsigned char)tb_s692[3] == 116 && tb_s692[4] == 0); })) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("*tb_p", v_id), " = tb_cat(*tb_p"), v_id), ", tb_v"), v_id), ");"));
    } else if ((({ const char *tb_s693 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 672))->q_ty); ((unsigned char)tb_s693[0] == 105 && (unsigned char)tb_s693[1] == 110 && (unsigned char)tb_s693[2] == 116 && tb_s693[3] == 0); }) && ({ const char *tb_s694 = v_op; ((unsigned char)tb_s694[0] == 47 && (unsigned char)tb_s694[1] == 61 && tb_s694[2] == 0); }))) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("*tb_p", v_id), " = tb_div(*tb_p"), v_id), ", tb_v"), v_id), ", "), tb_itos(((tb_o_Node *)tb_nn(v_n, 673))->q_line)), ");"));
    } else {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("*tb_p", v_id), " "), v_op), " tb_v"), v_id), ";"));
    }
    f_emit__put(v_c, " }\n");
}

static void f_emit__emit_multi(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    long long v_names = ((((tb_o_Node *)tb_nn(v_n, 679))->q_kids)->len - 1LL);
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 680))->q_kids, v_names, sizeof(tb_o_Node *), 680));
    f_emit__emit_pre(v_c, v_call, v_depth);
    { long long tb_v695 = 1LL; long long *tb_p695 = &(((tb_o_Comp *)tb_nn(v_c, 682))->q_label); *tb_p695 += tb_v695; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 683))->q_label));
    {
        long long tb_end696 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end696; v_i++) {
            if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 685))->q_kids, v_i, sizeof(tb_o_Node *), 685)), 685))->q_creates) {
                f_emit__indent(v_c, v_depth);
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 687))->q_kids, v_i, sizeof(tb_o_Node *), 687)), 687))->q_ty), " v_"), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 687))->q_kids, v_i, sizeof(tb_o_Node *), 687)), 687))->q_name), ";\n"));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_call, 691))->q_ty), " tb_m"), v_id), " = "));
    f_emit__emit_expr(v_c, v_call);
    f_emit__put(v_c, ";\n");
    {
        long long tb_end697 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end697; v_i++) {
            f_emit__indent(v_c, (v_depth + 1LL));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 696))->q_kids, v_i, sizeof(tb_o_Node *), 696)));
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(" = tb_m", v_id), ".o"), tb_itos(v_i)), ";\n"));
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_if(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    f_emit__indent(v_c, v_depth);
    long long v_i = 0LL;
    while (((v_i + 1LL) < (((tb_o_Node *)tb_nn(v_n, 704))->q_kids)->len)) {
        if ((v_i > 0LL)) {
            f_emit__put(v_c, " else ");
        }
        f_emit__put(v_c, "if (");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 708))->q_kids, v_i, sizeof(tb_o_Node *), 708)));
        f_emit__put(v_c, ") {\n");
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 710))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 710)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}");
        { long long tb_v698 = 2LL; long long *tb_p698 = &(v_i); *tb_p698 += tb_v698; }
    }
    if ((tb_mod((((tb_o_Node *)tb_nn(v_n, 714))->q_kids)->len, 2LL, 714) == 1LL)) {
        f_emit__put(v_c, " else {\n");
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 716))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 716))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 716)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}");
    }
    f_emit__put(v_c, "\n");
}

static void f_emit__emit_par(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v699 = 1LL; long long *tb_p699 = &(((tb_o_Comp *)tb_nn(v_c, 725))->q_label); *tb_p699 += tb_v699; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 726))->q_label));
    _Bool v_is_each = ({ const char *tb_s700 = ((tb_o_Node *)tb_nn(v_n, 727))->q_kind; ((unsigned char)tb_s700[0] == 101 && (unsigned char)tb_s700[1] == 97 && (unsigned char)tb_s700[2] == 99 && (unsigned char)tb_s700[3] == 104 && tb_s700[4] == 0); });
    const char * v_elem = "";
    if (v_is_each) {
        v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 730))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 730))->q_kids, 0LL, sizeof(tb_o_Node *), 730)), 730))->q_ty, sizeof(tb_o_Type *), 730)), 730))->q_elem);
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 732))->q_out;
    tb_list * v_w = tb_list_new(sizeof(const char *));
    { tb_list * tb_v701 = v_w; tb_list * *tb_p701 = &(((tb_o_Comp *)tb_nn(v_c, 734))->q_out); *tb_p701 = tb_v701; }
    const char * v_list_type = "tb_list *";
    if (v_is_each) {
        v_list_type = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 737))->q_kids, 0LL, sizeof(tb_o_Node *), 737)), 737))->q_ty);
    }
    f_emit__put(v_c, tb_cat(tb_cat("\ntypedef struct {\n    long long tb_lo, tb_total, tb_step, tb_next;\n    ", v_list_type), " tb_l;\n"));
    {
        tb_list * tb_l702 = ((tb_o_Node *)tb_nn(v_n, 739))->q_caps;
        for (long long tb_i702 = 0; tb_i702 < tb_l702->len; tb_i702++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l702->data)[tb_i702];
            if ((((tb_o_Var *)tb_nn(v_v, 740))->q_is_shared && ({ const char *tb_s703 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 740))->q_type); ((unsigned char)tb_s703[0] == 116 && (unsigned char)tb_s703[1] == 101 && (unsigned char)tb_s703[2] == 120 && (unsigned char)tb_s703[3] == 116 && tb_s703[4] == 0); }))) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("    const char *m_", ((tb_o_Var *)tb_nn(v_v, 742))->q_name), "; long long m_rl_"), ((tb_o_Var *)tb_nn(v_v, 742))->q_name), ", m_rc_"), ((tb_o_Var *)tb_nn(v_v, 742))->q_name), ";\n"));
            } else if (((tb_o_Var *)tb_nn(v_v, 743))->q_is_shared) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 744))->q_type)), " *p_"), ((tb_o_Var *)tb_nn(v_v, 744))->q_name), ";\n"));
            } else if (((tb_o_Var *)tb_nn(v_v, 745))->q_is_link) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 746))->q_type)), " *v_"), ((tb_o_Var *)tb_nn(v_v, 746))->q_name), ";\n"));
            } else {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 748))->q_type)), " v_"), ((tb_o_Var *)tb_nn(v_v, 748))->q_name), ";\n"));
            }
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat("} tb_job", v_id), ";\n"));
    f_emit__put(v_c, tb_cat(tb_cat("static void *tb_work", v_id), "(void *tb_arg) {\n"));
    f_emit__put(v_c, tb_cat(tb_cat("    tb_job", v_id), " *tb_j = tb_arg;\n"));
    _Bool v_has_shared = 0;
    tb_list * v_saved_builders = ((tb_o_Comp *)tb_nn(v_c, 754))->q_builders;
    tb_list * v_par_builders = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l704 = ((tb_o_Node *)tb_nn(v_n, 756))->q_caps;
        for (long long tb_i704 = 0; tb_i704 < tb_l704->len; tb_i704++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l704->data)[tb_i704];
            if (((tb_o_Var *)tb_nn(v_v, 757))->q_is_shared) {
                v_has_shared = 1;
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 759))->q_type)), " v_"), ((tb_o_Var *)tb_nn(v_v, 759))->q_name), " = "));
                f_emit__emit_default(v_c, ((tb_o_Var *)tb_nn(v_v, 760))->q_type);
                f_emit__put(v_c, ";\n");
                if (({ const char *tb_s705 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 762))->q_type); ((unsigned char)tb_s705[0] == 116 && (unsigned char)tb_s705[1] == 101 && (unsigned char)tb_s705[2] == 120 && (unsigned char)tb_s705[3] == 116 && tb_s705[4] == 0); })) {
                    { const char * tb_v706 = ((tb_o_Var *)tb_nn(v_v, 763))->q_name; *(const char * *)tb_grow(v_par_builders) = tb_v706; }
                    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    long long tb_rl_", ((tb_o_Var *)tb_nn(v_v, 764))->q_name), " = -1, tb_rc_"), ((tb_o_Var *)tb_nn(v_v, 764))->q_name), " = 0;\n"));
                }
            } else if (((tb_o_Var *)tb_nn(v_v, 765))->q_is_link) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 766))->q_type)), " *v_"), ((tb_o_Var *)tb_nn(v_v, 766))->q_name), " = tb_j->v_"), ((tb_o_Var *)tb_nn(v_v, 766))->q_name), ";\n"));
            } else {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 768))->q_type)), " v_"), ((tb_o_Var *)tb_nn(v_v, 768))->q_name), " = tb_j->v_"), ((tb_o_Var *)tb_nn(v_v, 768))->q_name), ";\n"));
            }
        }
    }
    { tb_list * tb_v707 = v_par_builders; tb_list * *tb_p707 = &(((tb_o_Comp *)tb_nn(v_c, 769))->q_builders); *tb_p707 = tb_v707; }
    f_emit__put(v_c, "    for (;;) {\n");
    f_emit__put(v_c, "        long long tb_s = __atomic_fetch_add(&tb_j->tb_next, tb_j->tb_step, __ATOMIC_RELAXED);\n");
    f_emit__put(v_c, "        if (tb_s >= tb_j->tb_total) break;\n");
    f_emit__put(v_c, "        long long tb_e = tb_s + tb_j->tb_step;\n");
    f_emit__put(v_c, "        if (tb_e > tb_j->tb_total) tb_e = tb_j->tb_total;\n");
    f_emit__put(v_c, "        for (long long tb_i = tb_s; tb_i < tb_e; tb_i++) {\n");
    if ((v_is_each && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 776))->q_kids, 0LL, sizeof(tb_o_Node *), 776)), 776))->q_ty))) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("            ", v_elem), " const v_"), ((tb_o_Node *)tb_nn(v_n, 777))->q_name), " = {tb_j->tb_l, tb_i};\n"));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 778))->q_kids, 1LL, sizeof(tb_o_Node *), 778)), 3LL);
    } else if ((v_is_each && f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 779))->q_kids, 0LL, sizeof(tb_o_Node *), 779)), 779))->q_ty))) {
        f_emit__put(v_c, tb_cat(tb_cat("            _Bool const v_", ((tb_o_Node *)tb_nn(v_n, 780))->q_name), " = TB_BIT(tb_j->tb_l, tb_i);\n"));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 781))->q_kids, 1LL, sizeof(tb_o_Node *), 781)), 3LL);
    } else if (v_is_each) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("            ", v_elem), " const v_"), ((tb_o_Node *)tb_nn(v_n, 783))->q_name), " = (("), v_elem), " *)tb_j->tb_l->data)[tb_i];\n"));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 784))->q_kids, 1LL, sizeof(tb_o_Node *), 784)), 3LL);
    } else {
        f_emit__put(v_c, tb_cat(tb_cat("            long long const v_", ((tb_o_Node *)tb_nn(v_n, 786))->q_name), " = tb_j->tb_lo + tb_i;\n"));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 787))->q_kids, 2LL, sizeof(tb_o_Node *), 787)), 3LL);
    }
    f_emit__put(v_c, "        }\n    }\n");
    { tb_list * tb_v708 = v_saved_builders; tb_list * *tb_p708 = &(((tb_o_Comp *)tb_nn(v_c, 789))->q_builders); *tb_p708 = tb_v708; }
    if (v_has_shared) {
        f_emit__put(v_c, "    pthread_mutex_lock(&tb_par_lock);\n");
        {
            tb_list * tb_l709 = ((tb_o_Node *)tb_nn(v_n, 793))->q_caps;
            for (long long tb_i709 = 0; tb_i709 < tb_l709->len; tb_i709++) {
                tb_o_Var * const v_v = ((tb_o_Var * *)tb_l709->data)[tb_i709];
                if ((((tb_o_Var *)tb_nn(v_v, 794))->q_is_shared && ({ const char *tb_s710 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 794))->q_type); ((unsigned char)tb_s710[0] == 116 && (unsigned char)tb_s710[1] == 101 && (unsigned char)tb_s710[2] == 120 && (unsigned char)tb_s710[3] == 116 && tb_s710[4] == 0); }))) {
                    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("    tb_app_take(&tb_j->m_", ((tb_o_Var *)tb_nn(v_v, 795))->q_name), ", &tb_j->m_rl_"), ((tb_o_Var *)tb_nn(v_v, 795))->q_name), ", &tb_j->m_rc_"), ((tb_o_Var *)tb_nn(v_v, 795))->q_name), ", v_"), ((tb_o_Var *)tb_nn(v_v, 795))->q_name), ", tb_rl_"), ((tb_o_Var *)tb_nn(v_v, 795))->q_name), ", tb_rc_"), ((tb_o_Var *)tb_nn(v_v, 795))->q_name), ");\n"));
                } else if (((tb_o_Var *)tb_nn(v_v, 796))->q_is_shared) {
                    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    *tb_j->p_", ((tb_o_Var *)tb_nn(v_v, 797))->q_name), " += v_"), ((tb_o_Var *)tb_nn(v_v, 797))->q_name), ";\n"));
                }
            }
        }
        f_emit__put(v_c, "    pthread_mutex_unlock(&tb_par_lock);\n");
    }
    f_emit__put(v_c, "    return 0;\n}\n");
    { const char * tb_v711 = tb_join(v_w, ""); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 800))->q_workers) = tb_v711; }
    { tb_list * tb_v712 = v_saved; tb_list * *tb_p712 = &(((tb_o_Comp *)tb_nn(v_c, 801))->q_out); *tb_p712 = tb_v712; }
    const char * v_j = tb_cat("tb_j", v_id);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("tb_job", v_id), " "), v_j), " = {0};\n"));
    f_emit__indent(v_c, (v_depth + 1LL));
    if (v_is_each) {
        f_emit__put(v_c, tb_cat(v_j, ".tb_l = "));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 811))->q_kids, 0LL, sizeof(tb_o_Node *), 811)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(v_j, ".tb_total = "), v_j), ".tb_l->len;\n"));
    } else {
        f_emit__put(v_c, tb_cat(tb_cat("long long tb_end", v_id), " = "));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 817))->q_kids, 1LL, sizeof(tb_o_Node *), 817)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, tb_cat(v_j, ".tb_lo = "));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 821))->q_kids, 0LL, sizeof(tb_o_Node *), 821)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_j, ".tb_total = tb_end"), v_id), " < "), v_j), ".tb_lo ? 0 : tb_end"), v_id), " - "), v_j), ".tb_lo + 1;\n"));
    }
    {
        tb_list * tb_l713 = ((tb_o_Node *)tb_nn(v_n, 825))->q_caps;
        for (long long tb_i713 = 0; tb_i713 < tb_l713->len; tb_i713++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l713->data)[tb_i713];
            f_emit__indent(v_c, (v_depth + 1LL));
            if ((((tb_o_Var *)tb_nn(v_v, 827))->q_is_shared && ({ const char *tb_s714 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 827))->q_type); ((unsigned char)tb_s714[0] == 116 && (unsigned char)tb_s714[1] == 101 && (unsigned char)tb_s714[2] == 120 && (unsigned char)tb_s714[3] == 116 && tb_s714[4] == 0); }))) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_j, ".m_"), ((tb_o_Var *)tb_nn(v_v, 828))->q_name), " = v_"), ((tb_o_Var *)tb_nn(v_v, 828))->q_name), "; "), v_j), ".m_rl_"), ((tb_o_Var *)tb_nn(v_v, 828))->q_name), " = -1;\n"));
            } else if (((tb_o_Var *)tb_nn(v_v, 829))->q_is_shared) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_j, ".p_"), ((tb_o_Var *)tb_nn(v_v, 830))->q_name), " = &v_"), ((tb_o_Var *)tb_nn(v_v, 830))->q_name), ";\n"));
            } else {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_j, ".v_"), ((tb_o_Var *)tb_nn(v_v, 832))->q_name), " = v_"), ((tb_o_Var *)tb_nn(v_v, 832))->q_name), ";\n"));
            }
        }
    }
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(v_j, ".tb_step = tb_par_step("), v_j), ".tb_total);\n"));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("tb_par_run(tb_work", v_id), ", &"), v_j), ", "), v_j), ".tb_total);\n"));
    {
        tb_list * tb_l715 = ((tb_o_Node *)tb_nn(v_n, 837))->q_caps;
        for (long long tb_i715 = 0; tb_i715 < tb_l715->len; tb_i715++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l715->data)[tb_i715];
            if ((((tb_o_Var *)tb_nn(v_v, 838))->q_is_shared && ({ const char *tb_s716 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 838))->q_type); ((unsigned char)tb_s716[0] == 116 && (unsigned char)tb_s716[1] == 101 && (unsigned char)tb_s716[2] == 120 && (unsigned char)tb_s716[3] == 116 && tb_s716[4] == 0); }))) {
                f_emit__indent(v_c, (v_depth + 1LL));
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("v_", ((tb_o_Var *)tb_nn(v_v, 840))->q_name), " = "), v_j), ".m_"), ((tb_o_Var *)tb_nn(v_v, 840))->q_name), ";\n"));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static long long f_emit__vec_index(tb_o_Comp * v_c, const char * v_name) {
    {
        long long tb_end717 = ((((tb_o_Comp *)tb_nn(v_c, 845))->q_vec_names)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end717; v_i++) {
            if ((strcmp((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 846))->q_vec_names, v_i, sizeof(const char *), 846)), v_name) == 0)) {
                return v_i;
            }
        }
    }
    return (-1LL);
    tb_crash(844, "функция vec_index дошла до конца без return");
    return 0LL;
}

static void f_emit__emit_vec(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v718 = 1LL; long long *tb_p718 = &(((tb_o_Comp *)tb_nn(v_c, 853))->q_label); *tb_p718 += tb_v718; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 854))->q_label));
    const char * v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 855))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 855))->q_kids, 0LL, sizeof(tb_o_Node *), 855)), 855))->q_ty, sizeof(tb_o_Type *), 855)), 855))->q_elem);
    long long v_d = (v_depth + 1LL);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, tb_cat(tb_cat("tb_list *tb_l", v_id), " = "));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 861))->q_kids, 0LL, sizeof(tb_o_Node *), 861)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_elem, " *tb_d"), v_id), " = ("), v_elem), " *)tb_l"), v_id), "->data;\n"));
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("long long tb_n", v_id), " = tb_l"), v_id), "->len, tb_i"), v_id), " = 0;\n"));
    {
        tb_list * tb_l719 = ((tb_o_Node *)tb_nn(v_n, 869))->q_caps;
        for (long long tb_i719 = 0; tb_i719 < tb_l719->len; tb_i719++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l719->data)[tb_i719];
            if (((tb_o_Var *)tb_nn(v_v, 870))->q_is_shared) {
                f_emit__indent(v_c, v_d);
                f_emit__put(v_c, tb_cat(f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 872))->q_type), " "));
                {
                    long long tb_end720 = 7LL;
                    for (long long v_k = 0LL; v_k <= tb_end720; v_k++) {
                        if ((v_k > 0LL)) {
                            f_emit__put(v_c, ", ");
                        }
                        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("tb_a", v_id), "_"), ((tb_o_Var *)tb_nn(v_v, 876))->q_name), "_"), tb_itos(v_k)), " = 0"));
                    }
                }
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("for (; tb_i", v_id), " + 8 <= tb_n"), v_id), "; tb_i"), v_id), " += 8) {\n"));
    {
        long long tb_end721 = 7LL;
        for (long long v_k = 0LL; v_k <= tb_end721; v_k++) {
            f_emit__indent(v_c, (v_d + 1LL));
            f_emit__put(v_c, "do {\n");
            f_emit__indent(v_c, (v_d + 2LL));
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_elem, " const v_"), ((tb_o_Node *)tb_nn(v_n, 886))->q_name), " = tb_d"), v_id), "[tb_i"), v_id), " + "), tb_itos(v_k)), "];\n"));
            {
                tb_list * tb_l722 = ((tb_o_Node *)tb_nn(v_n, 887))->q_caps;
                for (long long tb_i722 = 0; tb_i722 < tb_l722->len; tb_i722++) {
                    tb_o_Var * const v_v = ((tb_o_Var * *)tb_l722->data)[tb_i722];
                    if (((tb_o_Var *)tb_nn(v_v, 888))->q_is_shared) {
                        { const char * tb_v723 = ((tb_o_Var *)tb_nn(v_v, 889))->q_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 889))->q_vec_names) = tb_v723; }
                        { const char * tb_v724 = tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("tb_a", v_id), "_"), ((tb_o_Var *)tb_nn(v_v, 890))->q_name), "_"), tb_itos(v_k)); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 890))->q_vec_repl) = tb_v724; }
                    }
                }
            }
            f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 891))->q_kids, 1LL, sizeof(tb_o_Node *), 891)), (v_d + 2LL));
            tb_list * v_fresh_names = tb_list_new(sizeof(const char *));
            tb_list * v_fresh_repl = tb_list_new(sizeof(const char *));
            { tb_list * tb_v725 = v_fresh_names; tb_list * *tb_p725 = &(((tb_o_Comp *)tb_nn(v_c, 894))->q_vec_names); *tb_p725 = tb_v725; }
            { tb_list * tb_v726 = v_fresh_repl; tb_list * *tb_p726 = &(((tb_o_Comp *)tb_nn(v_c, 895))->q_vec_repl); *tb_p726 = tb_v726; }
            f_emit__indent(v_c, (v_d + 1LL));
            f_emit__put(v_c, "} while (0);\n");
        }
    }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("for (; tb_i", v_id), " < tb_n"), v_id), "; tb_i"), v_id), "++) {\n"));
    f_emit__indent(v_c, (v_d + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_elem, " const v_"), ((tb_o_Node *)tb_nn(v_n, 904))->q_name), " = tb_d"), v_id), "[tb_i"), v_id), "];\n"));
    {
        tb_list * tb_l727 = ((tb_o_Node *)tb_nn(v_n, 905))->q_caps;
        for (long long tb_i727 = 0; tb_i727 < tb_l727->len; tb_i727++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l727->data)[tb_i727];
            if (((tb_o_Var *)tb_nn(v_v, 906))->q_is_shared) {
                { const char * tb_v728 = ((tb_o_Var *)tb_nn(v_v, 907))->q_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 907))->q_vec_names) = tb_v728; }
                { const char * tb_v729 = tb_cat(tb_cat(tb_cat(tb_cat("tb_a", v_id), "_"), ((tb_o_Var *)tb_nn(v_v, 908))->q_name), "_0"); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 908))->q_vec_repl) = tb_v729; }
            }
        }
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 909))->q_kids, 1LL, sizeof(tb_o_Node *), 909)), (v_d + 1LL));
    tb_list * v_fresh_names2 = tb_list_new(sizeof(const char *));
    tb_list * v_fresh_repl2 = tb_list_new(sizeof(const char *));
    { tb_list * tb_v730 = v_fresh_names2; tb_list * *tb_p730 = &(((tb_o_Comp *)tb_nn(v_c, 912))->q_vec_names); *tb_p730 = tb_v730; }
    { tb_list * tb_v731 = v_fresh_repl2; tb_list * *tb_p731 = &(((tb_o_Comp *)tb_nn(v_c, 913))->q_vec_repl); *tb_p731 = tb_v731; }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, "}\n");
    {
        tb_list * tb_l732 = ((tb_o_Node *)tb_nn(v_n, 917))->q_caps;
        for (long long tb_i732 = 0; tb_i732 < tb_l732->len; tb_i732++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l732->data)[tb_i732];
            if (((tb_o_Var *)tb_nn(v_v, 918))->q_is_shared) {
                const char * v_target = tb_cat("v_", ((tb_o_Var *)tb_nn(v_v, 919))->q_name);
                if (((tb_o_Var *)tb_nn(v_v, 920))->q_is_link) {
                    v_target = tb_cat(tb_cat("(*v_", ((tb_o_Var *)tb_nn(v_v, 921))->q_name), ")");
                }
                const char * v_a = tb_cat(tb_cat(tb_cat(tb_cat("tb_a", v_id), "_"), ((tb_o_Var *)tb_nn(v_v, 922))->q_name), "_");
                f_emit__indent(v_c, v_d);
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_target, " += (("), v_a), "0 + "), v_a), "1) + ("), v_a), "2 + "), v_a), "3)) + (("), v_a), "4 + "), v_a), "5) + ("), v_a), "6 + "), v_a), "7));\n"));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_each(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    if (((tb_o_Node *)tb_nn(v_n, 929))->q_by_vec) {
        f_emit__emit_vec(v_c, v_n, v_depth);
        return;
    }
    if (((tb_o_Node *)tb_nn(v_n, 932))->q_is_par) {
        f_emit__emit_par(v_c, v_n, v_depth);
        return;
    }
    if (((tb_o_Node *)tb_nn(v_n, 935))->q_by_piece) {
        f_emit__emit_pieces(v_c, v_n, v_depth);
        return;
    }
    { long long tb_v733 = 1LL; long long *tb_p733 = &(((tb_o_Comp *)tb_nn(v_c, 938))->q_label); *tb_p733 += tb_v733; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 939))->q_label));
    long long v_t = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 940))->q_kids, 0LL, sizeof(tb_o_Node *), 940)), 940))->q_ty;
    const char * v_k = f_check__kind_of(v_c, v_t);
    long long v_elem_ty = (-1LL);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    if (({ const char *tb_s734 = v_k; ((unsigned char)tb_s734[0] == 100 && (unsigned char)tb_s734[1] == 105 && (unsigned char)tb_s734[2] == 99 && (unsigned char)tb_s734[3] == 116 && tb_s734[4] == 0); })) {
        f_emit__put(v_c, tb_cat(tb_cat("tb_list * tb_l", v_id), " = ("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 948))->q_kids, 0LL, sizeof(tb_o_Node *), 948)));
        f_emit__put(v_c, ")->keys;\n");
        v_k = "list";
        v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 951))->q_types, v_t, sizeof(tb_o_Type *), 951)), 951))->q_key;
    } else {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, v_t), " tb_l"), v_id), " = "));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 954))->q_kids, 0LL, sizeof(tb_o_Node *), 954)));
        f_emit__put(v_c, ";\n");
        if (({ const char *tb_s735 = v_k; ((unsigned char)tb_s735[0] == 108 && (unsigned char)tb_s735[1] == 105 && (unsigned char)tb_s735[2] == 115 && (unsigned char)tb_s735[3] == 116 && tb_s735[4] == 0); })) {
            v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 957))->q_types, v_t, sizeof(tb_o_Type *), 957)), 957))->q_elem;
        }
    }
    f_emit__indent(v_c, (v_depth + 1LL));
    if (({ const char *tb_s736 = v_k; ((unsigned char)tb_s736[0] == 116 && (unsigned char)tb_s736[1] == 101 && (unsigned char)tb_s736[2] == 120 && (unsigned char)tb_s736[3] == 116 && tb_s736[4] == 0); })) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("for (long long tb_i", v_id), " = 0; tb_l"), v_id), "[tb_i"), v_id), "] != 0; tb_i"), v_id), "++) {\n"));
        f_emit__indent(v_c, (v_depth + 2LL));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("const char * const v_", ((tb_o_Node *)tb_nn(v_n, 962))->q_name), " = tb_chr((unsigned char)tb_l"), v_id), "[tb_i"), v_id), "]);\n"));
    } else {
        const char * v_elem = f_emit__c_type(v_c, v_elem_ty);
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("for (long long tb_i", v_id), " = 0; tb_i"), v_id), " < tb_l"), v_id), "->len; tb_i"), v_id), "++) {\n"));
        f_emit__indent(v_c, (v_depth + 2LL));
        if (f_check__is_cols_obj(v_c, v_elem_ty)) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_elem, " const v_"), ((tb_o_Node *)tb_nn(v_n, 968))->q_name), " = {tb_l"), v_id), ", tb_i"), v_id), "};\n"));
        } else if ((v_elem_ty == 3LL)) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("_Bool const v_", ((tb_o_Node *)tb_nn(v_n, 970))->q_name), " = TB_BIT(tb_l"), v_id), ", tb_i"), v_id), ");\n"));
        } else {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_elem, " const v_"), ((tb_o_Node *)tb_nn(v_n, 972))->q_name), " = (("), v_elem), " *)tb_l"), v_id), "->data)[tb_i"), v_id), "];\n"));
            f_emit__emit_prefetch(v_c, v_n, (v_depth + 2LL), tb_cat("tb_l", v_id), tb_cat("tb_i", v_id), v_elem);
        }
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 974))->q_kids, 1LL, sizeof(tb_o_Node *), 974)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_pieces(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v737 = 1LL; long long *tb_p737 = &(((tb_o_Comp *)tb_nn(v_c, 985))->q_label); *tb_p737 += tb_v737; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 986))->q_label));
    const char * v_l = tb_cat("tb_l", v_id);
    const char * v_i = tb_cat("tb_i", v_id);
    const char * v_p = tb_cat("tb_p", v_id);
    const char * v_m = tb_cat("tb_n", v_id);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat("tb_list *", v_l), " = "));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 995))->q_kids, 0LL, sizeof(tb_o_Node *), 995)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat("long long ", v_m), " = "));
    f_emit__emit_expr(v_c, ((tb_o_Node *)tb_nn(v_n, 999))->q_piece_size);
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("if (", v_m), " <= 0) tb_crash("), tb_itos(((tb_o_Node *)tb_nn(v_n, 1002))->q_line)), ", \"размер куска в by должен быть больше 0\");\n"));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("tb_list ", v_p), " = {0, 0, -1, "), v_l), "->size};\n"));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("long long tb_cap", v_id), " = "), v_l), "->cap;\n"));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("if (", v_l), "->cap >= 0) "), v_l), "->cap = -2;\n"));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("for (long long ", v_i), " = 0; "), v_i), " < "), v_l), "->len; "), v_i), " += "), v_m), ") {\n"));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_p, ".data = "), v_l), "->data + "), v_i), " * "), v_l), "->size;\n"));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_p, ".len = "), v_l), "->len - "), v_i), " < "), v_m), " ? "), v_l), "->len - "), v_i), " : "), v_m), ";\n"));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("if (", v_i), " + "), v_m), " < "), v_l), "->len) __builtin_prefetch("), v_p), ".data + "), v_m), " * "), v_l), "->size);\n"));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("tb_list * const v_", ((tb_o_Node *)tb_nn(v_n, 1018))->q_name), " = &"), v_p), ";\n"));
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1019))->q_kids, 1LL, sizeof(tb_o_Node *), 1019)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(v_l, "->cap = tb_cap"), v_id), ";\n"));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_background(tb_o_Comp * v_c, tb_o_Node * v_n) {
    { long long tb_v738 = 1LL; long long *tb_p738 = &(((tb_o_Comp *)tb_nn(v_c, 1031))->q_label); *tb_p738 += tb_v738; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1032))->q_label));
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1033))->q_kids, 0LL, sizeof(tb_o_Node *), 1033));
    tb_o_Node * v_f = ((tb_o_Node *)tb_nn(v_call, 1034))->q_ref;
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1035))->q_kids, 0LL, sizeof(tb_o_Node *), 1035));
    long long v_rt = ((tb_o_Node *)tb_nn(v_f, 1036))->q_ty;
    if ((v_rt < 0LL)) {
        v_rt = 0LL;
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 1039))->q_out;
    tb_list * v_w = tb_list_new(sizeof(const char *));
    { tb_list * tb_v739 = v_w; tb_list * *tb_p739 = &(((tb_o_Comp *)tb_nn(v_c, 1041))->q_out); *tb_p739 = tb_v739; }
    f_emit__put(v_c, "\ntypedef struct {\n    tb_task tb_t;\n");
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, tb_cat(tb_cat("    ", f_emit__c_type(v_c, v_rt)), " tb_r;\n"));
    }
    {
        long long tb_end740 = ((((tb_o_Node *)tb_nn(v_params, 1045))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end740; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1046))->q_kids, v_i, sizeof(tb_o_Node *), 1046)), 1046))->q_ty)), " a"), tb_itos(v_i)), ";\n"));
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat("} tb_bgjob", v_id), ";\n"));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static void *tb_bgrun", v_id), "(void *tb_arg) {\n    tb_bgjob"), v_id), " *tb_j = tb_arg;\n    "));
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, "tb_j->tb_r = ");
    }
    f_emit__put(v_c, tb_cat(f_emit__fname(v_f), "("));
    {
        long long tb_end741 = ((((tb_o_Node *)tb_nn(v_params, 1052))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end741; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, tb_cat("tb_j->a", tb_itos(v_i)));
        }
    }
    f_emit__put(v_c, ");\n    return 0;\n}\n");
    f_emit__put(v_c, tb_cat(tb_cat("static tb_task *tb_bg", v_id), "("));
    {
        long long tb_end742 = ((((tb_o_Node *)tb_nn(v_params, 1058))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end742; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1061))->q_kids, v_i, sizeof(tb_o_Node *), 1061)), 1061))->q_ty), " a"), tb_itos(v_i)));
        }
    }
    if (((((tb_o_Node *)tb_nn(v_params, 1062))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(") {\n    tb_bgjob", v_id), " *tb_j = malloc(sizeof(tb_bgjob"), v_id), "));\n"));
    f_emit__put(v_c, "    if (tb_j == 0) tb_crash(0, \"не хватило памяти\");\n");
    {
        long long tb_end743 = ((((tb_o_Node *)tb_nn(v_params, 1066))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end743; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    tb_j->a", tb_itos(v_i)), " = a"), tb_itos(v_i)), ";\n"));
        }
    }
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, "    tb_j->tb_t.res = &tb_j->tb_r;\n");
    } else {
        f_emit__put(v_c, "    tb_j->tb_t.res = 0;\n");
    }
    f_emit__put(v_c, tb_cat(tb_cat("    tb_task_start(&tb_j->tb_t, tb_bgrun", v_id), ", tb_j);\n    return &tb_j->tb_t;\n}\n"));
    { tb_list * tb_v744 = v_saved; tb_list * *tb_p744 = &(((tb_o_Comp *)tb_nn(v_c, 1073))->q_out); *tb_p744 = tb_v744; }
    { const char * tb_v745 = tb_join(v_w, ""); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1074))->q_workers) = tb_v745; }
    f_emit__put(v_c, tb_cat(tb_cat("tb_bg", v_id), "("));
    {
        long long tb_end746 = ((((tb_o_Node *)tb_nn(v_call, 1076))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end746; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_call, 1079))->q_kids, v_i, sizeof(tb_o_Node *), 1079)), 1079))->q_kids, 0LL, sizeof(tb_o_Node *), 1079)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1079))->q_kids, v_i, sizeof(tb_o_Node *), 1079)), 1079))->q_ty);
        }
    }
    f_emit__put(v_c, ")");
}

static _Bool f_emit__is_pure(tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1084))->q_kind;
    if (((((({ const char *tb_s747 = v_k; ((unsigned char)tb_s747[0] == 110 && (unsigned char)tb_s747[1] == 117 && (unsigned char)tb_s747[2] == 109 && (unsigned char)tb_s747[3] == 98 && (unsigned char)tb_s747[4] == 101 && (unsigned char)tb_s747[5] == 114 && tb_s747[6] == 0); }) || ({ const char *tb_s748 = v_k; ((unsigned char)tb_s748[0] == 100 && (unsigned char)tb_s748[1] == 101 && (unsigned char)tb_s748[2] == 99 && (unsigned char)tb_s748[3] == 105 && (unsigned char)tb_s748[4] == 109 && (unsigned char)tb_s748[5] == 97 && (unsigned char)tb_s748[6] == 108 && tb_s748[7] == 0); })) || ({ const char *tb_s749 = v_k; ((unsigned char)tb_s749[0] == 115 && (unsigned char)tb_s749[1] == 116 && (unsigned char)tb_s749[2] == 114 && (unsigned char)tb_s749[3] == 105 && (unsigned char)tb_s749[4] == 110 && (unsigned char)tb_s749[5] == 103 && tb_s749[6] == 0); })) || ({ const char *tb_s750 = v_k; ((unsigned char)tb_s750[0] == 98 && (unsigned char)tb_s750[1] == 111 && (unsigned char)tb_s750[2] == 111 && (unsigned char)tb_s750[3] == 108 && tb_s750[4] == 0); })) || ({ const char *tb_s751 = v_k; ((unsigned char)tb_s751[0] == 110 && (unsigned char)tb_s751[1] == 97 && (unsigned char)tb_s751[2] == 109 && (unsigned char)tb_s751[3] == 101 && tb_s751[4] == 0); }))) {
        return 1;
    }
    if ((({ const char *tb_s752 = v_k; ((unsigned char)tb_s752[0] == 102 && (unsigned char)tb_s752[1] == 105 && (unsigned char)tb_s752[2] == 101 && (unsigned char)tb_s752[3] == 108 && (unsigned char)tb_s752[4] == 100 && tb_s752[5] == 0); }) || ({ const char *tb_s753 = v_k; ((unsigned char)tb_s753[0] == 117 && (unsigned char)tb_s753[1] == 110 && (unsigned char)tb_s753[2] == 97 && (unsigned char)tb_s753[3] == 114 && (unsigned char)tb_s753[4] == 121 && tb_s753[5] == 0); }))) {
        return f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1088))->q_kids, 0LL, sizeof(tb_o_Node *), 1088)));
    }
    if (({ const char *tb_s754 = v_k; ((unsigned char)tb_s754[0] == 98 && (unsigned char)tb_s754[1] == 105 && (unsigned char)tb_s754[2] == 110 && (unsigned char)tb_s754[3] == 111 && (unsigned char)tb_s754[4] == 112 && tb_s754[5] == 0); })) {
        if ((({ const char *tb_s755 = ((tb_o_Node *)tb_nn(v_n, 1090))->q_value; ((unsigned char)tb_s755[0] == 47 && tb_s755[1] == 0); }) || ({ const char *tb_s756 = ((tb_o_Node *)tb_nn(v_n, 1090))->q_value; ((unsigned char)tb_s756[0] == 37 && tb_s756[1] == 0); }))) {
            return 0;
        }
        return (f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1092))->q_kids, 0LL, sizeof(tb_o_Node *), 1092))) && f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1092))->q_kids, 1LL, sizeof(tb_o_Node *), 1092))));
    }
    return 0;
    tb_crash(1083, "функция is_pure дошла до конца без return");
    return 0;
}

static void f_emit__emit_together(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v757 = 1LL; long long *tb_p757 = &(((tb_o_Comp *)tb_nn(v_c, 1101))->q_label); *tb_p757 += tb_v757; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1102))->q_label));
    tb_o_Node * v_block = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1103))->q_kids, 0LL, sizeof(tb_o_Node *), 1103));
    {
        long long tb_end758 = ((((tb_o_Node *)tb_nn(v_block, 1104))->q_kids)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end758; v_j++) {
            tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_block, 1105))->q_kids, v_j, sizeof(tb_o_Node *), 1105)), 1105))->q_kids, 0LL, sizeof(tb_o_Node *), 1105));
            if (((({ const char *tb_s759 = ((tb_o_Node *)tb_nn(v_v, 1106))->q_kind; !((unsigned char)tb_s759[0] == 105 && (unsigned char)tb_s759[1] == 110 && (unsigned char)tb_s759[2] == 100 && (unsigned char)tb_s759[3] == 101 && (unsigned char)tb_s759[4] == 120 && tb_s759[5] == 0); }) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1106))->q_kids, 0LL, sizeof(tb_o_Node *), 1106))))) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1106))->q_kids, 1LL, sizeof(tb_o_Node *), 1106)))))) {
                continue;
            }
            f_emit__indent(v_c, v_depth);
            if (({ const char *tb_s760 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1109))->q_kids, 0LL, sizeof(tb_o_Node *), 1109)), 1109))->q_ty); ((unsigned char)tb_s760[0] == 100 && (unsigned char)tb_s760[1] == 105 && (unsigned char)tb_s760[2] == 99 && (unsigned char)tb_s760[3] == 116 && tb_s760[4] == 0); })) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("unsigned long long tb_h", v_id), "_"), tb_itos(v_j)), " = tb_dpre1("));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1111))->q_kids, 0LL, sizeof(tb_o_Node *), 1111)));
                f_emit__put(v_c, ", ");
                f_emit__emit_key(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1113))->q_kids, 0LL, sizeof(tb_o_Node *), 1113)), 1113))->q_ty, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1113))->q_kids, 1LL, sizeof(tb_o_Node *), 1113)));
                f_emit__put(v_c, ");\n");
            } else if ((f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1115))->q_kids, 0LL, sizeof(tb_o_Node *), 1115)), 1115))->q_ty) && (!f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1115))->q_kids, 0LL, sizeof(tb_o_Node *), 1115)), 1115))->q_ty)))) {
                f_emit__put(v_c, "tb_lpre(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1117))->q_kids, 0LL, sizeof(tb_o_Node *), 1117)));
                f_emit__put(v_c, ", ");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1119))->q_kids, 1LL, sizeof(tb_o_Node *), 1119)));
                f_emit__put(v_c, ");\n");
            } else {
                f_emit__put(v_c, ";\n");
            }
        }
    }
    {
        long long tb_end761 = ((((tb_o_Node *)tb_nn(v_block, 1123))->q_kids)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end761; v_j++) {
            tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_block, 1124))->q_kids, v_j, sizeof(tb_o_Node *), 1124)), 1124))->q_kids, 0LL, sizeof(tb_o_Node *), 1124));
            if (((({ const char *tb_s762 = ((tb_o_Node *)tb_nn(v_v, 1125))->q_kind; !((unsigned char)tb_s762[0] == 105 && (unsigned char)tb_s762[1] == 110 && (unsigned char)tb_s762[2] == 100 && (unsigned char)tb_s762[3] == 101 && (unsigned char)tb_s762[4] == 120 && tb_s762[5] == 0); }) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1125))->q_kids, 0LL, sizeof(tb_o_Node *), 1125))))) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1125))->q_kids, 1LL, sizeof(tb_o_Node *), 1125)))))) {
                continue;
            }
            if (({ const char *tb_s763 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1127))->q_kids, 0LL, sizeof(tb_o_Node *), 1127)), 1127))->q_ty); ((unsigned char)tb_s763[0] == 100 && (unsigned char)tb_s763[1] == 105 && (unsigned char)tb_s763[2] == 99 && (unsigned char)tb_s763[3] == 116 && tb_s763[4] == 0); })) {
                f_emit__indent(v_c, v_depth);
                f_emit__put(v_c, "tb_dpre2(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1130))->q_kids, 0LL, sizeof(tb_o_Node *), 1130)));
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(", tb_h", v_id), "_"), tb_itos(v_j)), ");\n"));
            }
        }
    }
    {
        tb_list * tb_l764 = ((tb_o_Node *)tb_nn(v_block, 1132))->q_kids;
        for (long long tb_i764 = 0; tb_i764 < tb_l764->len; tb_i764++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l764->data)[tb_i764];
            f_emit__emit_statement(v_c, v_s, v_depth);
        }
    }
}

static void f_emit__emit_prefetch(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth, const char * v_list_c, const char * v_i_c, const char * v_elem) {
    {
        long long tb_end765 = ((((tb_o_Node *)tb_nn(v_n, 1139))->q_pre_bases)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end765; v_j++) {
            { long long tb_v766 = 1LL; long long *tb_p766 = &(((tb_o_Comp *)tb_nn(v_c, 1140))->q_label); *tb_p766 += tb_v766; }
            const char * v_f = tb_cat("tb_f", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1141))->q_label));
            const char * v_plan = v_list_c;
            f_emit__indent(v_c, v_depth);
            if (({ const char *tb_s767 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1144))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1144)), 1144))->q_kind; ((unsigned char)tb_s767[0] == 110 && (unsigned char)tb_s767[1] == 97 && (unsigned char)tb_s767[2] == 109 && (unsigned char)tb_s767[3] == 101 && tb_s767[4] == 0); })) {
                f_emit__put(v_c, tb_cat(tb_cat("{ tb_list *tb_q", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1145))->q_label)), " = "));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1146))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1146)));
                f_emit__put(v_c, "; ");
                v_plan = tb_cat("tb_q", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1148))->q_label));
            } else {
                f_emit__put(v_c, "{ ");
            }
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("if ((unsigned long long)(", v_i_c), " + 16) < (unsigned long long)"), v_plan), "->len) { "));
            const char * v_el = v_elem;
            if (({ const char *tb_s768 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1153))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1153)), 1153))->q_kind; ((unsigned char)tb_s768[0] == 110 && (unsigned char)tb_s768[1] == 97 && (unsigned char)tb_s768[2] == 109 && (unsigned char)tb_s768[3] == 101 && tb_s768[4] == 0); })) {
                v_el = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1154))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1154))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1154)), 1154))->q_ty, sizeof(tb_o_Type *), 1154)), 1154))->q_elem);
            }
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_el, " "), v_f), " = (("), v_el), " *)"), v_plan), "->data)["), v_i_c), " + 16]; "));
            tb_o_Node * v_base = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1156))->q_pre_bases, v_j, sizeof(tb_o_Node *), 1156));
            if (({ const char *tb_s769 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_base, 1157))->q_ty); ((unsigned char)tb_s769[0] == 100 && (unsigned char)tb_s769[1] == 105 && (unsigned char)tb_s769[2] == 99 && (unsigned char)tb_s769[3] == 116 && tb_s769[4] == 0); })) {
                f_emit__put(v_c, "(void)tb_dpre1(");
                f_emit__emit_expr(v_c, v_base);
                f_emit__put(v_c, tb_cat(tb_cat(", &", v_f), "); } }\n"));
            } else {
                f_emit__put(v_c, "tb_lpre(");
                f_emit__emit_expr(v_c, v_base);
                f_emit__put(v_c, tb_cat(tb_cat(", ", v_f), "); } }\n"));
            }
        }
    }
}

static _Bool f_emit__short_literal(tb_o_Node * v_n) {
    if (((({ const char *tb_s770 = ((tb_o_Node *)tb_nn(v_n, 1168))->q_kind; !((unsigned char)tb_s770[0] == 115 && (unsigned char)tb_s770[1] == 116 && (unsigned char)tb_s770[2] == 114 && (unsigned char)tb_s770[3] == 105 && (unsigned char)tb_s770[4] == 110 && (unsigned char)tb_s770[5] == 103 && tb_s770[6] == 0); }) || (tb_tlen(((tb_o_Node *)tb_nn(v_n, 1168))->q_value) < 2LL)) || (tb_tlen(((tb_o_Node *)tb_nn(v_n, 1168))->q_value) > 10LL))) {
        return 0;
    }
    {
        const char * tb_l771 = ((tb_o_Node *)tb_nn(v_n, 1170))->q_value;
        for (long long tb_i771 = 0; tb_l771[tb_i771] != 0; tb_i771++) {
            const char * const v_ch = tb_chr((unsigned char)tb_l771[tb_i771]);
            if ((strcmp(v_ch, "\\") == 0)) {
                return 0;
            }
        }
    }
    return 1;
    tb_crash(1167, "функция short_literal дошла до конца без return");
    return 0;
}

static void f_emit__emit_count(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    if (((tb_o_Node *)tb_nn(v_n, 1176))->q_is_par) {
        f_emit__emit_par(v_c, v_n, v_depth);
        return;
    }
    { long long tb_v772 = 1LL; long long *tb_p772 = &(((tb_o_Comp *)tb_nn(v_c, 1179))->q_label); *tb_p772 += tb_v772; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1180))->q_label));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat("long long tb_end", v_id), " = "));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1185))->q_kids, 1LL, sizeof(tb_o_Node *), 1185)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat("for (long long v_", ((tb_o_Node *)tb_nn(v_n, 1188))->q_name), " = "));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1189))->q_kids, 0LL, sizeof(tb_o_Node *), 1189)));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("; v_", ((tb_o_Node *)tb_nn(v_n, 1190))->q_name), " <= tb_end"), v_id), "; v_"), ((tb_o_Node *)tb_nn(v_n, 1190))->q_name), "++) {\n"));
    f_emit__emit_prefetch(v_c, v_n, (v_depth + 2LL), "", tb_cat("v_", ((tb_o_Node *)tb_nn(v_n, 1191))->q_name), "long long");
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1192))->q_kids, 2LL, sizeof(tb_o_Node *), 1192)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_gives_return(tb_o_Comp * v_c, long long v_depth) {
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1200))->q_cur_func, 1200))->q_kids, 2LL, sizeof(tb_o_Node *), 1200));
    f_emit__indent(v_c, v_depth);
    if (((((tb_o_Node *)tb_nn(v_outputs, 1202))->q_kids)->len == 1LL)) {
        f_emit__put(v_c, tb_cat(tb_cat("return v_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1203))->q_kids, 0LL, sizeof(tb_o_Node *), 1203)), 1203))->q_name), ";\n"));
        return;
    }
    f_emit__put(v_c, tb_cat(tb_cat("return (", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1205))->q_cur_func, 1205))->q_ty)), "){"));
    {
        long long tb_end773 = ((((tb_o_Node *)tb_nn(v_outputs, 1206))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end773; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, tb_cat("v_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1209))->q_kids, v_i, sizeof(tb_o_Node *), 1209)), 1209))->q_name));
        }
    }
    f_emit__put(v_c, "};\n");
}

static _Bool f_emit__fill_form(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_body = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1214))->q_kids, 1LL, sizeof(tb_o_Node *), 1214));
    if ((((((tb_o_Node *)tb_nn(v_body, 1215))->q_kids)->len != 1LL) || (strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_body, 1215))->q_kids, 0LL, sizeof(tb_o_Node *), 1215)), 1215))->q_kind, "expr_stmt") != 0))) {
        return 0;
    }
    tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_body, 1217))->q_kids, 0LL, sizeof(tb_o_Node *), 1217)), 1217))->q_kids, 0LL, sizeof(tb_o_Node *), 1217));
    if ((((({ const char *tb_s774 = ((tb_o_Node *)tb_nn(v_e, 1218))->q_kind; !((unsigned char)tb_s774[0] == 109 && (unsigned char)tb_s774[1] == 101 && (unsigned char)tb_s774[2] == 116 && (unsigned char)tb_s774[3] == 104 && (unsigned char)tb_s774[4] == 111 && (unsigned char)tb_s774[5] == 100 && tb_s774[6] == 0); }) || ({ const char *tb_s775 = ((tb_o_Node *)tb_nn(v_e, 1218))->q_name; !((unsigned char)tb_s775[0] == 97 && (unsigned char)tb_s775[1] == 100 && (unsigned char)tb_s775[2] == 100 && tb_s775[3] == 0); })) || ({ const char *tb_s776 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1218))->q_kids, 0LL, sizeof(tb_o_Node *), 1218)), 1218))->q_kind; !((unsigned char)tb_s776[0] == 110 && (unsigned char)tb_s776[1] == 97 && (unsigned char)tb_s776[2] == 109 && (unsigned char)tb_s776[3] == 101 && tb_s776[4] == 0); })) || f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1218))->q_kids, 0LL, sizeof(tb_o_Node *), 1218)), 1218))->q_ty))) {
        return 0;
    }
    tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1220))->q_kids, 1LL, sizeof(tb_o_Node *), 1220)), 1220))->q_kids, 0LL, sizeof(tb_o_Node *), 1220));
    const char * v_k = ((tb_o_Node *)tb_nn(v_v, 1221))->q_kind;
    return (((({ const char *tb_s777 = v_k; ((unsigned char)tb_s777[0] == 110 && (unsigned char)tb_s777[1] == 117 && (unsigned char)tb_s777[2] == 109 && (unsigned char)tb_s777[3] == 98 && (unsigned char)tb_s777[4] == 101 && (unsigned char)tb_s777[5] == 114 && tb_s777[6] == 0); }) || ({ const char *tb_s778 = v_k; ((unsigned char)tb_s778[0] == 100 && (unsigned char)tb_s778[1] == 101 && (unsigned char)tb_s778[2] == 99 && (unsigned char)tb_s778[3] == 105 && (unsigned char)tb_s778[4] == 109 && (unsigned char)tb_s778[5] == 97 && (unsigned char)tb_s778[6] == 108 && tb_s778[7] == 0); })) || ({ const char *tb_s779 = v_k; ((unsigned char)tb_s779[0] == 115 && (unsigned char)tb_s779[1] == 116 && (unsigned char)tb_s779[2] == 114 && (unsigned char)tb_s779[3] == 105 && (unsigned char)tb_s779[4] == 110 && (unsigned char)tb_s779[5] == 103 && tb_s779[6] == 0); })) || ({ const char *tb_s780 = v_k; ((unsigned char)tb_s780[0] == 98 && (unsigned char)tb_s780[1] == 111 && (unsigned char)tb_s780[2] == 111 && (unsigned char)tb_s780[3] == 108 && tb_s780[4] == 0); }));
    tb_crash(1213, "функция fill_form дошла до конца без return");
    return 0;
}

static void f_emit__emit_statement(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1225))->q_kind;
    if (({ const char *tb_s781 = v_k; ((unsigned char)tb_s781[0] == 97 && (unsigned char)tb_s781[1] == 115 && (unsigned char)tb_s781[2] == 115 && (unsigned char)tb_s781[3] == 105 && (unsigned char)tb_s781[4] == 103 && (unsigned char)tb_s781[5] == 110 && tb_s781[6] == 0); })) {
        f_emit__emit_assign(v_c, v_n, v_depth);
    } else if (({ const char *tb_s782 = v_k; ((unsigned char)tb_s782[0] == 115 && (unsigned char)tb_s782[1] == 101 && (unsigned char)tb_s782[2] == 116 && tb_s782[3] == 0); })) {
        f_emit__emit_set(v_c, v_n, v_depth);
    } else if (({ const char *tb_s783 = v_k; ((unsigned char)tb_s783[0] == 108 && (unsigned char)tb_s783[1] == 105 && (unsigned char)tb_s783[2] == 110 && (unsigned char)tb_s783[3] == 107 && (unsigned char)tb_s783[4] == 100 && (unsigned char)tb_s783[5] == 101 && (unsigned char)tb_s783[6] == 99 && (unsigned char)tb_s783[7] == 108 && tb_s783[8] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 1232))->q_ty), " *v_"), ((tb_o_Node *)tb_nn(v_n, 1232))->q_name), " = &("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1233))->q_kids, 0LL, sizeof(tb_o_Node *), 1233)));
        f_emit__put(v_c, ");\n");
    } else if (({ const char *tb_s784 = v_k; ((unsigned char)tb_s784[0] == 109 && (unsigned char)tb_s784[1] == 117 && (unsigned char)tb_s784[2] == 108 && (unsigned char)tb_s784[3] == 116 && (unsigned char)tb_s784[4] == 105 && tb_s784[5] == 0); })) {
        f_emit__emit_multi(v_c, v_n, v_depth);
    } else if (({ const char *tb_s785 = v_k; ((unsigned char)tb_s785[0] == 105 && (unsigned char)tb_s785[1] == 102 && tb_s785[2] == 0); })) {
        f_emit__emit_if(v_c, v_n, v_depth);
    } else if (({ const char *tb_s786 = v_k; ((unsigned char)tb_s786[0] == 101 && (unsigned char)tb_s786[1] == 97 && (unsigned char)tb_s786[2] == 99 && (unsigned char)tb_s786[3] == 104 && tb_s786[4] == 0); })) {
        f_emit__emit_each(v_c, v_n, v_depth);
    } else if (({ const char *tb_s787 = v_k; ((unsigned char)tb_s787[0] == 99 && (unsigned char)tb_s787[1] == 111 && (unsigned char)tb_s787[2] == 117 && (unsigned char)tb_s787[3] == 110 && (unsigned char)tb_s787[4] == 116 && tb_s787[5] == 0); })) {
        f_emit__emit_count(v_c, v_n, v_depth);
    } else if (((strcmp(v_k, "repeat_times") == 0) && f_emit__fill_form(v_c, v_n))) {
        tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1245))->q_kids, 1LL, sizeof(tb_o_Node *), 1245)), 1245))->q_kids, 0LL, sizeof(tb_o_Node *), 1245)), 1245))->q_kids, 0LL, sizeof(tb_o_Node *), 1245));
        tb_o_Node * v_lst = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1246))->q_kids, 0LL, sizeof(tb_o_Node *), 1246));
        tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1247))->q_kids, 1LL, sizeof(tb_o_Node *), 1247)), 1247))->q_kids, 0LL, sizeof(tb_o_Node *), 1247));
        f_emit__indent(v_c, v_depth);
        if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_lst, 1249))->q_ty)) {
            f_emit__put(v_c, "tb_bfill(");
            f_emit__emit_expr(v_c, v_lst);
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1253))->q_kids, 0LL, sizeof(tb_o_Node *), 1253)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, v_value);
            f_emit__put(v_c, ");\n");
        } else {
            long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1258))->q_types, ((tb_o_Node *)tb_nn(v_lst, 1258))->q_ty, sizeof(tb_o_Type *), 1258)), 1258))->q_elem;
            f_emit__put(v_c, "tb_fill(");
            f_emit__emit_expr(v_c, v_lst);
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1262))->q_kids, 0LL, sizeof(tb_o_Node *), 1262)));
            f_emit__put(v_c, tb_cat(tb_cat(", &(", f_emit__c_type(v_c, v_elem)), "){"));
            f_emit__emit_as(v_c, v_value, v_elem);
            f_emit__put(v_c, "});\n");
        }
    } else if ((strcmp(v_k, "repeat_times") == 0)) {
        { long long tb_v788 = 1LL; long long *tb_p788 = &(((tb_o_Comp *)tb_nn(v_c, 1267))->q_label); *tb_p788 += tb_v788; }
        const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1268))->q_label));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("for (long long tb_r", v_id), " = 0, tb_n"), v_id), " = "));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1271))->q_kids, 0LL, sizeof(tb_o_Node *), 1271)));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("; tb_r", v_id), " < tb_n"), v_id), "; tb_r"), v_id), "++) {\n"));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1273))->q_kids, 1LL, sizeof(tb_o_Node *), 1273)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if ((strcmp(v_k, "repeat_while") == 0)) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "while (");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1279))->q_kids, 0LL, sizeof(tb_o_Node *), 1279)));
        f_emit__put(v_c, ") {\n");
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1281))->q_kids, 1LL, sizeof(tb_o_Node *), 1281)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if (({ const char *tb_s789 = v_k; ((unsigned char)tb_s789[0] == 116 && (unsigned char)tb_s789[1] == 111 && (unsigned char)tb_s789[2] == 103 && (unsigned char)tb_s789[3] == 101 && (unsigned char)tb_s789[4] == 116 && (unsigned char)tb_s789[5] == 104 && (unsigned char)tb_s789[6] == 101 && (unsigned char)tb_s789[7] == 114 && tb_s789[8] == 0); })) {
        f_emit__emit_together(v_c, v_n, v_depth);
    } else if (({ const char *tb_s790 = v_k; ((unsigned char)tb_s790[0] == 116 && (unsigned char)tb_s790[1] == 101 && (unsigned char)tb_s790[2] == 109 && (unsigned char)tb_s790[3] == 112 && tb_s790[4] == 0); })) {
        { long long tb_v791 = 1LL; long long *tb_p791 = &(((tb_o_Comp *)tb_nn(v_c, 1287))->q_label); *tb_p791 += tb_v791; }
        const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1288))->q_label));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "{\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, tb_cat(tb_cat("tb_mark tb_k", v_id), " = tb_temp_begin();\n"));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1293))->q_kids, 0LL, sizeof(tb_o_Node *), 1293)), (v_depth + 1LL));
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, tb_cat(tb_cat("tb_temp_end(tb_k", v_id), ");\n"));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if (({ const char *tb_s792 = v_k; ((unsigned char)tb_s792[0] == 115 && (unsigned char)tb_s792[1] == 107 && (unsigned char)tb_s792[2] == 105 && (unsigned char)tb_s792[3] == 112 && tb_s792[4] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "continue;\n");
    } else if (({ const char *tb_s793 = v_k; ((unsigned char)tb_s793[0] == 115 && (unsigned char)tb_s793[1] == 116 && (unsigned char)tb_s793[2] == 111 && (unsigned char)tb_s793[3] == 112 && tb_s793[4] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "break;\n");
    } else if (({ const char *tb_s794 = v_k; ((unsigned char)tb_s794[0] == 102 && (unsigned char)tb_s794[1] == 97 && (unsigned char)tb_s794[2] == 105 && (unsigned char)tb_s794[3] == 108 && tb_s794[4] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "{ tb_err = ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1307))->q_kids, 0LL, sizeof(tb_o_Node *), 1307)));
        f_emit__put(v_c, "; ");
        f_emit__emit_bail(v_c);
        f_emit__put(v_c, " }\n");
    } else if (({ const char *tb_s795 = v_k; ((unsigned char)tb_s795[0] == 114 && (unsigned char)tb_s795[1] == 101 && (unsigned char)tb_s795[2] == 116 && (unsigned char)tb_s795[3] == 117 && (unsigned char)tb_s795[4] == 114 && (unsigned char)tb_s795[5] == 110 && tb_s795[6] == 0); })) {
        if (((((tb_o_Node *)tb_nn(v_n, 1312))->q_kids)->len == 1LL)) {
            f_emit__emit_pre(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1313))->q_kids, 0LL, sizeof(tb_o_Node *), 1313)), v_depth);
        }
        if ((((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1314))->q_cur_func, 1314))->q_kids, 2LL, sizeof(tb_o_Node *), 1314)), 1314))->q_kids)->len > 0LL) && ((((tb_o_Node *)tb_nn(v_n, 1314))->q_kids)->len == 0LL))) {
            f_emit__emit_gives_return(v_c, v_depth);
        } else if (((((tb_o_Node *)tb_nn(v_n, 1316))->q_kids)->len == 1LL)) {
            f_emit__indent(v_c, v_depth);
            f_emit__put(v_c, "return ");
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1319))->q_kids, 0LL, sizeof(tb_o_Node *), 1319)), ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1319))->q_cur_func, 1319))->q_ty);
            f_emit__put(v_c, ";\n");
        } else {
            f_emit__indent(v_c, v_depth);
            f_emit__put(v_c, "return;\n");
        }
    } else if ((strcmp(v_k, "expr_stmt") == 0)) {
        tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1325))->q_kids, 0LL, sizeof(tb_o_Node *), 1325));
        if ((({ const char *tb_s796 = ((tb_o_Node *)tb_nn(v_e, 1326))->q_kind; ((unsigned char)tb_s796[0] == 116 && (unsigned char)tb_s796[1] == 114 && (unsigned char)tb_s796[2] == 121 && tb_s796[3] == 0); }) || ({ const char *tb_s797 = ((tb_o_Node *)tb_nn(v_e, 1326))->q_kind; ((unsigned char)tb_s797[0] == 101 && (unsigned char)tb_s797[1] == 108 && (unsigned char)tb_s797[2] == 115 && (unsigned char)tb_s797[3] == 101 && (unsigned char)tb_s797[4] == 95 && (unsigned char)tb_s797[5] == 101 && (unsigned char)tb_s797[6] == 114 && (unsigned char)tb_s797[7] == 114 && tb_s797[8] == 0); }))) {
            f_emit__emit_pre(v_c, v_e, v_depth);
            return;
        }
        f_emit__indent(v_c, v_depth);
        if ((({ const char *tb_s798 = ((tb_o_Node *)tb_nn(v_e, 1330))->q_kind; ((unsigned char)tb_s798[0] == 109 && (unsigned char)tb_s798[1] == 101 && (unsigned char)tb_s798[2] == 116 && (unsigned char)tb_s798[3] == 104 && (unsigned char)tb_s798[4] == 111 && (unsigned char)tb_s798[5] == 100 && tb_s798[6] == 0); }) && ({ const char *tb_s799 = ((tb_o_Node *)tb_nn(v_e, 1330))->q_name; ((unsigned char)tb_s799[0] == 97 && (unsigned char)tb_s799[1] == 100 && (unsigned char)tb_s799[2] == 100 && tb_s799[3] == 0); }))) {
            long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1332))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1332))->q_kids, 0LL, sizeof(tb_o_Node *), 1332)), 1332))->q_ty, sizeof(tb_o_Type *), 1332)), 1332))->q_elem;
            { long long tb_v800 = 1LL; long long *tb_p800 = &(((tb_o_Comp *)tb_nn(v_c, 1333))->q_label); *tb_p800 += tb_v800; }
            const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1334))->q_label));
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1335))->q_kids, 1LL, sizeof(tb_o_Node *), 1335)), 1335))->q_kids, 0LL, sizeof(tb_o_Node *), 1335));
            if ((((f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1336))->q_kids, 0LL, sizeof(tb_o_Node *), 1336)), 1336))->q_ty) && ({ const char *tb_s801 = ((tb_o_Node *)tb_nn(v_value, 1336))->q_kind; ((unsigned char)tb_s801[0] == 99 && (unsigned char)tb_s801[1] == 97 && (unsigned char)tb_s801[2] == 108 && (unsigned char)tb_s801[3] == 108 && tb_s801[4] == 0); })) && ((tb_o_Node *)tb_nn(v_value, 1336))->q_has_ref) && ({ const char *tb_s802 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_value, 1336))->q_ref, 1336))->q_kind; ((unsigned char)tb_s802[0] == 111 && (unsigned char)tb_s802[1] == 98 && (unsigned char)tb_s802[2] == 106 && tb_s802[3] == 0); }))) {
                tb_o_Node * v_o = ((tb_o_Node *)tb_nn(v_value, 1338))->q_ref;
                f_emit__put(v_c, tb_cat(tb_cat("tb_colpush_", ((tb_o_Node *)tb_nn(v_o, 1339))->q_name), "("));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1340))->q_kids, 0LL, sizeof(tb_o_Node *), 1340)));
                {
                    long long tb_end803 = ((((tb_o_Node *)tb_nn(v_o, 1341))->q_kids)->len - 1LL);
                    for (long long v_i = 0LL; v_i <= tb_end803; v_i++) {
                        f_emit__put(v_c, ", ");
                        if (({ const char *tb_s804 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 1343))->q_kids, v_i, sizeof(tb_o_Node *), 1343)), 1343))->q_kind; ((unsigned char)tb_s804[0] == 109 && (unsigned char)tb_s804[1] == 105 && (unsigned char)tb_s804[2] == 115 && (unsigned char)tb_s804[3] == 115 && (unsigned char)tb_s804[4] == 105 && (unsigned char)tb_s804[5] == 110 && (unsigned char)tb_s804[6] == 103 && tb_s804[7] == 0); })) {
                            f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1344))->q_kids, v_i, sizeof(tb_o_Node *), 1344)), 1344))->q_ty);
                        } else {
                            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 1346))->q_kids, v_i, sizeof(tb_o_Node *), 1346)), 1346))->q_kids, 0LL, sizeof(tb_o_Node *), 1346)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1346))->q_kids, v_i, sizeof(tb_o_Node *), 1346)), 1346))->q_ty);
                        }
                    }
                }
                f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_e, 1347))->q_line)), ");\n"));
                return;
            }
            if (f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1349))->q_kids, 0LL, sizeof(tb_o_Node *), 1349)), 1349))->q_ty)) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("{ ", f_emit__c_type(v_c, v_elem)), " tb_v"), v_id), " = "));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1351))->q_kids, 1LL, sizeof(tb_o_Node *), 1351)), 1351))->q_kids, 0LL, sizeof(tb_o_Node *), 1351)));
                f_emit__put(v_c, tb_cat(tb_cat("; tb_coladd_", f_emit__cols_name(v_c, v_elem)), "("));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1353))->q_kids, 0LL, sizeof(tb_o_Node *), 1353)));
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(", tb_v", v_id), ", "), tb_itos(((tb_o_Node *)tb_nn(v_e, 1354))->q_line)), "); }\n"));
                return;
            }
            if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1356))->q_kids, 0LL, sizeof(tb_o_Node *), 1356)), 1356))->q_ty)) {
                f_emit__put(v_c, tb_cat(tb_cat("{ _Bool tb_v", v_id), " = "));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1358))->q_kids, 1LL, sizeof(tb_o_Node *), 1358)), 1358))->q_kids, 0LL, sizeof(tb_o_Node *), 1358)));
                f_emit__put(v_c, "; tb_badd(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1360))->q_kids, 0LL, sizeof(tb_o_Node *), 1360)));
                f_emit__put(v_c, tb_cat(tb_cat(", tb_v", v_id), "); }\n"));
                return;
            }
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("{ ", f_emit__c_type(v_c, v_elem)), " tb_v"), v_id), " = "));
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1364))->q_kids, 1LL, sizeof(tb_o_Node *), 1364)), 1364))->q_kids, 0LL, sizeof(tb_o_Node *), 1364)), v_elem);
            f_emit__put(v_c, tb_cat(tb_cat("; *(", f_emit__c_type(v_c, v_elem)), " *)tb_grow("));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1366))->q_kids, 0LL, sizeof(tb_o_Node *), 1366)));
            f_emit__put(v_c, tb_cat(tb_cat(") = tb_v", v_id), "; }\n"));
        } else {
            f_emit__emit_expr(v_c, v_e);
            f_emit__put(v_c, ";\n");
        }
    }
}

static void f_emit__emit_block(tb_o_Comp * v_c, tb_o_Node * v_block, long long v_depth) {
    {
        tb_list * tb_l805 = ((tb_o_Node *)tb_nn(v_block, 1373))->q_kids;
        for (long long tb_i805 = 0; tb_i805 < tb_l805->len; tb_i805++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l805->data)[tb_i805];
            f_emit__emit_statement(v_c, v_s, v_depth);
        }
    }
}

static void f_emit__emit_signature(tb_o_Comp * v_c, tb_o_Node * v_f) {
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1379))->q_kids, 0LL, sizeof(tb_o_Node *), 1379));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1380))->q_ty)), " "), f_emit__fname(v_f)), "("));
    if (((((tb_o_Node *)tb_nn(v_params, 1381))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end806 = ((((tb_o_Node *)tb_nn(v_params, 1383))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end806; v_i++) {
            tb_o_Node * v_p = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1384))->q_kids, v_i, sizeof(tb_o_Node *), 1384));
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            if (((tb_o_Node *)tb_nn(v_p, 1387))->q_is_link) {
                f_emit__put(v_c, tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1388))->q_ty), " *v_"), ((tb_o_Node *)tb_nn(v_p, 1388))->q_name));
            } else {
                f_emit__put(v_c, tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1390))->q_ty), " v_"), ((tb_o_Node *)tb_nn(v_p, 1390))->q_name));
            }
        }
    }
    f_emit__put(v_c, ")");
}

static _Bool f_emit__bld_has(tb_list * v_names, const char * v_name) {
    {
        tb_list * tb_l807 = v_names;
        for (long long tb_i807 = 0; tb_i807 < tb_l807->len; tb_i807++) {
            const char * const v_x = ((const char * *)tb_l807->data)[tb_i807];
            if ((strcmp(v_x, v_name) == 0)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1400, "функция bld_has дошла до конца без return");
    return 0;
}

static _Bool f_emit__bld_uses(tb_o_Node * v_n, const char * v_name) {
    if ((({ const char *tb_s808 = ((tb_o_Node *)tb_nn(v_n, 1407))->q_kind; ((unsigned char)tb_s808[0] == 110 && (unsigned char)tb_s808[1] == 97 && (unsigned char)tb_s808[2] == 109 && (unsigned char)tb_s808[3] == 101 && tb_s808[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1407))->q_name, v_name) == 0))) {
        return 1;
    }
    {
        tb_list * tb_l809 = ((tb_o_Node *)tb_nn(v_n, 1409))->q_kids;
        for (long long tb_i809 = 0; tb_i809 < tb_l809->len; tb_i809++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l809->data)[tb_i809];
            if (f_emit__bld_uses(v_k, v_name)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1406, "функция bld_uses дошла до конца без return");
    return 0;
}

static void f_emit__bld_bad_all(tb_o_Node * v_n, tb_list * v_bad) {
    if ((({ const char *tb_s810 = ((tb_o_Node *)tb_nn(v_n, 1416))->q_kind; ((unsigned char)tb_s810[0] == 110 && (unsigned char)tb_s810[1] == 97 && (unsigned char)tb_s810[2] == 109 && (unsigned char)tb_s810[3] == 101 && tb_s810[4] == 0); }) || ({ const char *tb_s811 = ((tb_o_Node *)tb_nn(v_n, 1416))->q_kind; ((unsigned char)tb_s811[0] == 97 && (unsigned char)tb_s811[1] == 115 && (unsigned char)tb_s811[2] == 115 && (unsigned char)tb_s811[3] == 105 && (unsigned char)tb_s811[4] == 103 && (unsigned char)tb_s811[5] == 110 && tb_s811[6] == 0); }))) {
        { const char * tb_v812 = ((tb_o_Node *)tb_nn(v_n, 1417))->q_name; *(const char * *)tb_grow(v_bad) = tb_v812; }
    }
    {
        tb_list * tb_l813 = ((tb_o_Node *)tb_nn(v_n, 1418))->q_kids;
        for (long long tb_i813 = 0; tb_i813 < tb_l813->len; tb_i813++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l813->data)[tb_i813];
            f_emit__bld_bad_all(v_k, v_bad);
        }
    }
}

static _Bool f_emit__is_text_plus(tb_o_Comp * v_c, tb_o_Node * v_n) {
    return ((({ const char *tb_s814 = ((tb_o_Node *)tb_nn(v_n, 1422))->q_kind; ((unsigned char)tb_s814[0] == 98 && (unsigned char)tb_s814[1] == 105 && (unsigned char)tb_s814[2] == 110 && (unsigned char)tb_s814[3] == 111 && (unsigned char)tb_s814[4] == 112 && tb_s814[5] == 0); }) && ({ const char *tb_s815 = ((tb_o_Node *)tb_nn(v_n, 1422))->q_value; ((unsigned char)tb_s815[0] == 43 && tb_s815[1] == 0); })) && ({ const char *tb_s816 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 1422))->q_ty); ((unsigned char)tb_s816[0] == 116 && (unsigned char)tb_s816[1] == 101 && (unsigned char)tb_s816[2] == 120 && (unsigned char)tb_s816[3] == 116 && tb_s816[4] == 0); }));
    tb_crash(1421, "функция is_text_plus дошла до конца без return");
    return 0;
}

static _Bool f_emit__app_form(tb_o_Comp * v_c, const char * v_name, tb_o_Node * v_v) {
    if ((!f_emit__is_text_plus(v_c, v_v))) {
        return 0;
    }
    tb_o_Node * v_r = v_v;
    while (f_emit__is_text_plus(v_c, v_r)) {
        if (f_emit__bld_uses((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1430))->q_kids, 1LL, sizeof(tb_o_Node *), 1430)), v_name)) {
            return 0;
        }
        v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1432))->q_kids, 0LL, sizeof(tb_o_Node *), 1432));
    }
    return ((({ const char *tb_s817 = ((tb_o_Node *)tb_nn(v_r, 1433))->q_kind; ((unsigned char)tb_s817[0] == 110 && (unsigned char)tb_s817[1] == 97 && (unsigned char)tb_s817[2] == 109 && (unsigned char)tb_s817[3] == 101 && tb_s817[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_r, 1433))->q_name, v_name) == 0)) && (!((tb_o_Node *)tb_nn(v_r, 1433))->q_is_link));
    tb_crash(1425, "функция app_form дошла до конца без return");
    return 0;
}

static tb_list * f_emit__app_parts(tb_o_Comp * v_c, tb_o_Node * v_v) {
    tb_list * v_parts = tb_list_new(sizeof(tb_o_Node *));
    tb_list * v_back = tb_list_new(sizeof(tb_o_Node *));
    tb_o_Node * v_r = v_v;
    while (f_emit__is_text_plus(v_c, v_r)) {
        { tb_o_Node * tb_v818 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1440))->q_kids, 1LL, sizeof(tb_o_Node *), 1440)); *(tb_o_Node * *)tb_grow(v_back) = tb_v818; }
        v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1441))->q_kids, 0LL, sizeof(tb_o_Node *), 1441));
    }
    long long v_i = ((v_back)->len - 1LL);
    while ((v_i >= 0LL)) {
        { tb_o_Node * tb_v819 = (*(tb_o_Node * *)tb_at(v_back, v_i, sizeof(tb_o_Node *), 1444)); *(tb_o_Node * *)tb_grow(v_parts) = tb_v819; }
        { long long tb_v820 = 1LL; long long *tb_p820 = &(v_i); *tb_p820 -= tb_v820; }
    }
    return v_parts;
}

static void f_emit__bld_scan(tb_o_Comp * v_c, tb_o_Node * v_n, _Bool v_in_par, tb_list * v_cand, tb_list * v_bad) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1448))->q_kind;
    _Bool v_par = (((v_in_par || (strcmp(v_k, "background") == 0)) || ({ const char *tb_s821 = v_k; ((unsigned char)tb_s821[0] == 116 && (unsigned char)tb_s821[1] == 111 && (unsigned char)tb_s821[2] == 103 && (unsigned char)tb_s821[3] == 101 && (unsigned char)tb_s821[4] == 116 && (unsigned char)tb_s821[5] == 104 && (unsigned char)tb_s821[6] == 101 && (unsigned char)tb_s821[7] == 114 && tb_s821[8] == 0); })) || ((({ const char *tb_s822 = v_k; ((unsigned char)tb_s822[0] == 101 && (unsigned char)tb_s822[1] == 97 && (unsigned char)tb_s822[2] == 99 && (unsigned char)tb_s822[3] == 104 && tb_s822[4] == 0); }) || ({ const char *tb_s823 = v_k; ((unsigned char)tb_s823[0] == 99 && (unsigned char)tb_s823[1] == 111 && (unsigned char)tb_s823[2] == 117 && (unsigned char)tb_s823[3] == 110 && (unsigned char)tb_s823[4] == 116 && tb_s823[5] == 0); })) && (((tb_o_Node *)tb_nn(v_n, 1449))->q_is_par || ((tb_o_Node *)tb_nn(v_n, 1449))->q_by_vec)));
    if (v_par) {
        f_emit__bld_bad_all(v_n, v_bad);
        return;
    }
    if ((({ const char *tb_s824 = v_k; ((unsigned char)tb_s824[0] == 108 && (unsigned char)tb_s824[1] == 105 && (unsigned char)tb_s824[2] == 110 && (unsigned char)tb_s824[3] == 107 && (unsigned char)tb_s824[4] == 100 && (unsigned char)tb_s824[5] == 101 && (unsigned char)tb_s824[6] == 99 && (unsigned char)tb_s824[7] == 108 && tb_s824[8] == 0); }) || ({ const char *tb_s825 = v_k; ((unsigned char)tb_s825[0] == 109 && (unsigned char)tb_s825[1] == 117 && (unsigned char)tb_s825[2] == 108 && (unsigned char)tb_s825[3] == 116 && (unsigned char)tb_s825[4] == 105 && tb_s825[5] == 0); }))) {
        f_emit__bld_bad_all(v_n, v_bad);
    }
    if (((({ const char *tb_s826 = v_k; ((unsigned char)tb_s826[0] == 101 && (unsigned char)tb_s826[1] == 97 && (unsigned char)tb_s826[2] == 99 && (unsigned char)tb_s826[3] == 104 && tb_s826[4] == 0); }) || ({ const char *tb_s827 = v_k; ((unsigned char)tb_s827[0] == 99 && (unsigned char)tb_s827[1] == 111 && (unsigned char)tb_s827[2] == 117 && (unsigned char)tb_s827[3] == 110 && (unsigned char)tb_s827[4] == 116 && tb_s827[5] == 0); })) || ({ const char *tb_s828 = v_k; ((unsigned char)tb_s828[0] == 101 && (unsigned char)tb_s828[1] == 108 && (unsigned char)tb_s828[2] == 115 && (unsigned char)tb_s828[3] == 101 && (unsigned char)tb_s828[4] == 95 && (unsigned char)tb_s828[5] == 101 && (unsigned char)tb_s828[6] == 114 && (unsigned char)tb_s828[7] == 114 && tb_s828[8] == 0); }))) {
        { const char * tb_v829 = ((tb_o_Node *)tb_nn(v_n, 1456))->q_name; *(const char * *)tb_grow(v_bad) = tb_v829; }
    }
    if (((({ const char *tb_s830 = v_k; ((unsigned char)tb_s830[0] == 99 && (unsigned char)tb_s830[1] == 97 && (unsigned char)tb_s830[2] == 108 && (unsigned char)tb_s830[3] == 108 && tb_s830[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1457))->q_has_ref) && ({ const char *tb_s831 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1457))->q_ref, 1457))->q_kind; ((unsigned char)tb_s831[0] == 102 && (unsigned char)tb_s831[1] == 117 && (unsigned char)tb_s831[2] == 110 && (unsigned char)tb_s831[3] == 99 && tb_s831[4] == 0); }))) {
        tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1458))->q_ref, 1458))->q_kids, 0LL, sizeof(tb_o_Node *), 1458));
        {
            long long tb_end832 = ((((tb_o_Node *)tb_nn(v_n, 1459))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end832; v_i++) {
                if (((v_i < (((tb_o_Node *)tb_nn(v_params, 1460))->q_kids)->len) && ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1460))->q_kids, v_i, sizeof(tb_o_Node *), 1460)), 1460))->q_is_link)) {
                    f_emit__bld_bad_all((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1461))->q_kids, v_i, sizeof(tb_o_Node *), 1461)), v_bad);
                }
            }
        }
    }
    if (({ const char *tb_s833 = v_k; ((unsigned char)tb_s833[0] == 97 && (unsigned char)tb_s833[1] == 115 && (unsigned char)tb_s833[2] == 115 && (unsigned char)tb_s833[3] == 105 && (unsigned char)tb_s833[4] == 103 && (unsigned char)tb_s833[5] == 110 && tb_s833[6] == 0); })) {
        if (((tb_o_Node *)tb_nn(v_n, 1463))->q_is_link) {
            { const char * tb_v834 = ((tb_o_Node *)tb_nn(v_n, 1464))->q_name; *(const char * *)tb_grow(v_bad) = tb_v834; }
        } else if ((((({ const char *tb_s835 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 1465))->q_ty); ((unsigned char)tb_s835[0] == 116 && (unsigned char)tb_s835[1] == 101 && (unsigned char)tb_s835[2] == 120 && (unsigned char)tb_s835[3] == 116 && tb_s835[4] == 0); }) && ((((tb_o_Node *)tb_nn(v_n, 1465))->q_kids)->len == 1LL)) && (!((tb_o_Node *)tb_nn(v_n, 1465))->q_creates)) && f_emit__app_form(v_c, ((tb_o_Node *)tb_nn(v_n, 1465))->q_name, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1465))->q_kids, 0LL, sizeof(tb_o_Node *), 1465))))) {
            { const char * tb_v836 = ((tb_o_Node *)tb_nn(v_n, 1466))->q_name; *(const char * *)tb_grow(v_cand) = tb_v836; }
        }
    }
    if ((((({ const char *tb_s837 = v_k; ((unsigned char)tb_s837[0] == 115 && (unsigned char)tb_s837[1] == 101 && (unsigned char)tb_s837[2] == 116 && tb_s837[3] == 0); }) && ({ const char *tb_s838 = ((tb_o_Node *)tb_nn(v_n, 1467))->q_value; ((unsigned char)tb_s838[0] == 43 && (unsigned char)tb_s838[1] == 61 && tb_s838[2] == 0); })) && ({ const char *tb_s839 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1467))->q_kids, 0LL, sizeof(tb_o_Node *), 1467)), 1467))->q_kind; ((unsigned char)tb_s839[0] == 110 && (unsigned char)tb_s839[1] == 97 && (unsigned char)tb_s839[2] == 109 && (unsigned char)tb_s839[3] == 101 && tb_s839[4] == 0); })) && ({ const char *tb_s840 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1467))->q_kids, 0LL, sizeof(tb_o_Node *), 1467)), 1467))->q_ty); ((unsigned char)tb_s840[0] == 116 && (unsigned char)tb_s840[1] == 101 && (unsigned char)tb_s840[2] == 120 && (unsigned char)tb_s840[3] == 116 && tb_s840[4] == 0); }))) {
        if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1468))->q_kids, 0LL, sizeof(tb_o_Node *), 1468)), 1468))->q_is_link) {
            { const char * tb_v841 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1469))->q_kids, 0LL, sizeof(tb_o_Node *), 1469)), 1469))->q_name; *(const char * *)tb_grow(v_bad) = tb_v841; }
        } else {
            { const char * tb_v842 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1471))->q_kids, 0LL, sizeof(tb_o_Node *), 1471)), 1471))->q_name; *(const char * *)tb_grow(v_cand) = tb_v842; }
        }
    }
    {
        tb_list * tb_l843 = ((tb_o_Node *)tb_nn(v_n, 1472))->q_kids;
        for (long long tb_i843 = 0; tb_i843 < tb_l843->len; tb_i843++) {
            tb_o_Node * const v_kid = ((tb_o_Node * *)tb_l843->data)[tb_i843];
            f_emit__bld_scan(v_c, v_kid, 0, v_cand, v_bad);
        }
    }
}

static void f_emit__find_builders(tb_o_Comp * v_c, tb_o_Node * v_f) {
    tb_list * v_fresh = tb_list_new(sizeof(const char *));
    { tb_list * tb_v844 = v_fresh; tb_list * *tb_p844 = &(((tb_o_Comp *)tb_nn(v_c, 1477))->q_builders); *tb_p844 = tb_v844; }
    tb_list * v_cand = tb_list_new(sizeof(const char *));
    tb_list * v_bad = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l845 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1480))->q_kids, 0LL, sizeof(tb_o_Node *), 1480)), 1480))->q_kids;
        for (long long tb_i845 = 0; tb_i845 < tb_l845->len; tb_i845++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l845->data)[tb_i845];
            if (((tb_o_Node *)tb_nn(v_p, 1481))->q_is_link) {
                { const char * tb_v846 = ((tb_o_Node *)tb_nn(v_p, 1482))->q_name; *(const char * *)tb_grow(v_bad) = tb_v846; }
            }
        }
    }
    f_emit__bld_scan(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1483))->q_kids, 1LL, sizeof(tb_o_Node *), 1483)), 0, v_cand, v_bad);
    {
        tb_list * tb_l847 = v_cand;
        for (long long tb_i847 = 0; tb_i847 < tb_l847->len; tb_i847++) {
            const char * const v_name = ((const char * *)tb_l847->data)[tb_i847];
            if (((!f_emit__bld_has(v_bad, v_name)) && (!f_emit__bld_has(((tb_o_Comp *)tb_nn(v_c, 1485))->q_builders, v_name)))) {
                { const char * tb_v848 = v_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1486))->q_builders) = tb_v848; }
            }
        }
    }
}

static _Bool f_emit__is_bld(tb_o_Comp * v_c, tb_o_Node * v_n) {
    return ((((({ const char *tb_s849 = ((tb_o_Node *)tb_nn(v_n, 1490))->q_kind; ((unsigned char)tb_s849[0] == 110 && (unsigned char)tb_s849[1] == 97 && (unsigned char)tb_s849[2] == 109 && (unsigned char)tb_s849[3] == 101 && tb_s849[4] == 0); }) && (!((tb_o_Node *)tb_nn(v_n, 1490))->q_is_link)) && ((((tb_o_Comp *)tb_nn(v_c, 1490))->q_builders)->len > 0LL)) && ({ const char *tb_s850 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 1490))->q_ty); ((unsigned char)tb_s850[0] == 116 && (unsigned char)tb_s850[1] == 101 && (unsigned char)tb_s850[2] == 120 && (unsigned char)tb_s850[3] == 116 && tb_s850[4] == 0); })) && f_emit__bld_has(((tb_o_Comp *)tb_nn(v_c, 1490))->q_builders, ((tb_o_Node *)tb_nn(v_n, 1490))->q_name));
    tb_crash(1489, "функция is_bld дошла до конца без return");
    return 0;
}

static void f_emit__emit_q(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if (f_emit__is_bld(v_c, v_n)) {
        f_emit__put(v_c, tb_cat("v_", ((tb_o_Node *)tb_nn(v_n, 1495))->q_name));
    } else {
        f_emit__emit_expr(v_c, v_n);
    }
}

static void f_emit__emit_app(tb_o_Comp * v_c, const char * v_name, tb_o_Node * v_part) {
    const char * v_vars = tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("&v_", v_name), ", &tb_rl_"), v_name), ", &tb_rc_"), v_name), ", ");
    if (({ const char *tb_s851 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_part, 1501))->q_ty); ((unsigned char)tb_s851[0] == 105 && (unsigned char)tb_s851[1] == 110 && (unsigned char)tb_s851[2] == 116 && tb_s851[3] == 0); })) {
        f_emit__put(v_c, tb_cat("tb_app_i(", v_vars));
        f_emit__emit_expr(v_c, v_part);
        f_emit__put(v_c, ");");
    } else if (({ const char *tb_s852 = ((tb_o_Node *)tb_nn(v_part, 1506))->q_kind; ((unsigned char)tb_s852[0] == 115 && (unsigned char)tb_s852[1] == 116 && (unsigned char)tb_s852[2] == 114 && (unsigned char)tb_s852[3] == 105 && (unsigned char)tb_s852[4] == 110 && (unsigned char)tb_s852[5] == 103 && tb_s852[6] == 0); })) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("tb_app_n(", v_vars), ((tb_o_Node *)tb_nn(v_part, 1508))->q_value), ", sizeof("), ((tb_o_Node *)tb_nn(v_part, 1508))->q_value), ") - 1);"));
    } else {
        f_emit__put(v_c, tb_cat("tb_app(", v_vars));
        f_emit__emit_as_text(v_c, v_part);
        f_emit__put(v_c, ");");
    }
}

static void f_emit__emit_func(tb_o_Comp * v_c, tb_o_Node * v_f) {
    { tb_o_Node * tb_v853 = v_f; tb_o_Node * *tb_p853 = &(((tb_o_Comp *)tb_nn(v_c, 1515))->q_cur_func); *tb_p853 = tb_v853; }
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1516))->q_kids, 2LL, sizeof(tb_o_Node *), 1516));
    f_emit__put(v_c, "\n");
    f_emit__emit_signature(v_c, v_f);
    f_emit__put(v_c, " {\n");
    {
        tb_list * tb_l854 = ((tb_o_Node *)tb_nn(v_outputs, 1520))->q_kids;
        for (long long tb_i854 = 0; tb_i854 < tb_l854->len; tb_i854++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l854->data)[tb_i854];
            if ((!((tb_o_Node *)tb_nn(v_o, 1521))->q_is_const)) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_o, 1522))->q_ty)), " v_"), ((tb_o_Node *)tb_nn(v_o, 1522))->q_name), " = "));
                f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_o, 1523))->q_ty);
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__find_builders(v_c, v_f);
    {
        tb_list * tb_l855 = ((tb_o_Comp *)tb_nn(v_c, 1526))->q_builders;
        for (long long tb_i855 = 0; tb_i855 < tb_l855->len; tb_i855++) {
            const char * const v_b = ((const char * *)tb_l855->data)[tb_i855];
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("    long long tb_rl_", v_b), " = -1, tb_rc_"), v_b), " = 0; (void)tb_rl_"), v_b), "; (void)tb_rc_"), v_b), ";\n"));
        }
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1528))->q_kids, 1LL, sizeof(tb_o_Node *), 1528)), 1LL);
    tb_list * v_no_builders = tb_list_new(sizeof(const char *));
    { tb_list * tb_v856 = v_no_builders; tb_list * *tb_p856 = &(((tb_o_Comp *)tb_nn(v_c, 1530))->q_builders); *tb_p856 = tb_v856; }
    if (((((tb_o_Node *)tb_nn(v_outputs, 1531))->q_kids)->len > 0LL)) {
        f_emit__emit_gives_return(v_c, 1LL);
    } else if (({ const char *tb_s857 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 1533))->q_ty); !((unsigned char)tb_s857[0] == 110 && (unsigned char)tb_s857[1] == 111 && (unsigned char)tb_s857[2] == 110 && (unsigned char)tb_s857[3] == 101 && tb_s857[4] == 0); })) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    tb_crash(", tb_itos(((tb_o_Node *)tb_nn(v_f, 1534))->q_line)), ", \"функция "), ((tb_o_Node *)tb_nn(v_f, 1534))->q_name), " дошла до конца без return\");\n"));
        f_emit__put(v_c, "    return ");
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_f, 1536))->q_ty);
        f_emit__put(v_c, ";\n");
    }
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_show(tb_o_Comp * v_c, tb_o_Type * v_t) {
    const char * v_k = ((tb_o_Type *)tb_nn(v_t, 1542))->q_kind;
    if (((({ const char *tb_s858 = v_k; ((unsigned char)tb_s858[0] == 110 && (unsigned char)tb_s858[1] == 111 && (unsigned char)tb_s858[2] == 110 && (unsigned char)tb_s858[3] == 101 && tb_s858[4] == 0); }) || ({ const char *tb_s859 = v_k; ((unsigned char)tb_s859[0] == 109 && (unsigned char)tb_s859[1] == 117 && (unsigned char)tb_s859[2] == 108 && (unsigned char)tb_s859[3] == 116 && (unsigned char)tb_s859[4] == 105 && tb_s859[5] == 0); })) || ({ const char *tb_s860 = v_k; ((unsigned char)tb_s860[0] == 117 && (unsigned char)tb_s860[1] == 110 && (unsigned char)tb_s860[2] == 107 && (unsigned char)tb_s860[3] == 110 && (unsigned char)tb_s860[4] == 111 && (unsigned char)tb_s860[5] == 119 && (unsigned char)tb_s860[6] == 110 && tb_s860[7] == 0); }))) {
        return;
    }
    if (({ const char *tb_s861 = v_k; ((unsigned char)tb_s861[0] == 116 && (unsigned char)tb_s861[1] == 97 && (unsigned char)tb_s861[2] == 115 && (unsigned char)tb_s861[3] == 107 && tb_s861[4] == 0); })) {
        f_emit__put(v_c, tb_cat(tb_cat("static void tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1546))->q_id)), "(tb_task *x) { (void)x; tb_ps(\"<background>\"); }\n"));
        return;
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static void tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1548))->q_id)), "("), f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1548))->q_id)), " x) {\n"));
    if (({ const char *tb_s862 = v_k; ((unsigned char)tb_s862[0] == 105 && (unsigned char)tb_s862[1] == 110 && (unsigned char)tb_s862[2] == 116 && tb_s862[3] == 0); })) {
        f_emit__put(v_c, "    printf(\"%lld\", x);\n");
    } else if (({ const char *tb_s863 = v_k; ((unsigned char)tb_s863[0] == 100 && (unsigned char)tb_s863[1] == 101 && (unsigned char)tb_s863[2] == 99 && tb_s863[3] == 0); })) {
        f_emit__put(v_c, "    printf(\"%g\", x);\n");
    } else if (({ const char *tb_s864 = v_k; ((unsigned char)tb_s864[0] == 98 && (unsigned char)tb_s864[1] == 111 && (unsigned char)tb_s864[2] == 111 && (unsigned char)tb_s864[3] == 108 && tb_s864[4] == 0); })) {
        f_emit__put(v_c, "    tb_ps(x ? \"true\" : \"false\");\n");
    } else if (({ const char *tb_s865 = v_k; ((unsigned char)tb_s865[0] == 116 && (unsigned char)tb_s865[1] == 101 && (unsigned char)tb_s865[2] == 120 && (unsigned char)tb_s865[3] == 116 && tb_s865[4] == 0); })) {
        f_emit__put(v_c, "    tb_ps(x);\n");
    } else if (({ const char *tb_s866 = v_k; ((unsigned char)tb_s866[0] == 108 && (unsigned char)tb_s866[1] == 105 && (unsigned char)tb_s866[2] == 115 && (unsigned char)tb_s866[3] == 116 && tb_s866[4] == 0); })) {
        const char * v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1558))->q_elem);
        f_emit__put(v_c, "    tb_ps(\"[\");\n");
        f_emit__put(v_c, "    for (long long i = 0; i < x->len; i++) {\n");
        f_emit__put(v_c, "        if (i > 0) tb_ps(\", \");\n");
        if (f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn(v_t, 1562))->q_elem)) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("        tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1563))->q_elem)), "(("), v_elem), "){x, i});\n"));
        } else if ((((tb_o_Type *)tb_nn(v_t, 1564))->q_elem == 3LL)) {
            f_emit__put(v_c, "        tb_show_3(TB_BIT(x, i));\n");
        } else {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("        tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1567))->q_elem)), "((("), v_elem), " *)x->data)[i]);\n"));
        }
        f_emit__put(v_c, "    }\n");
        f_emit__put(v_c, "    tb_ps(\"]\");\n");
    } else if (({ const char *tb_s867 = v_k; ((unsigned char)tb_s867[0] == 100 && (unsigned char)tb_s867[1] == 105 && (unsigned char)tb_s867[2] == 99 && (unsigned char)tb_s867[3] == 116 && tb_s867[4] == 0); })) {
        const char * v_kt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1571))->q_key);
        const char * v_vt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1572))->q_elem);
        f_emit__put(v_c, "    tb_ps(\"{\");\n");
        f_emit__put(v_c, "    for (long long i = 0; i < x->keys->len; i++) {\n");
        f_emit__put(v_c, "        if (i > 0) tb_ps(\", \");\n");
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("        tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1576))->q_key)), "((("), v_kt), " *)x->keys->data)[i]);\n"));
        f_emit__put(v_c, "        tb_ps(\": \");\n");
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("        tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1578))->q_elem)), "((("), v_vt), " *)x->vals->data)[i]);\n"));
        f_emit__put(v_c, "    }\n");
        f_emit__put(v_c, "    tb_ps(\"}\");\n");
    } else if (({ const char *tb_s868 = v_k; ((unsigned char)tb_s868[0] == 111 && (unsigned char)tb_s868[1] == 98 && (unsigned char)tb_s868[2] == 106 && tb_s868[3] == 0); })) {
        tb_o_Node * v_o = ((tb_o_Type *)tb_nn(v_t, 1582))->q_obj_node;
        if (((tb_o_Node *)tb_nn(v_o, 1583))->q_by_cols) {
            f_emit__put(v_c, "    if (x.c == 0) { tb_ps(\"пусто\"); return; }\n");
        } else {
            f_emit__put(v_c, "    if (x == 0) { tb_ps(\"пусто\"); return; }\n");
        }
        f_emit__put(v_c, tb_cat(tb_cat("    tb_ps(\"", ((tb_o_Node *)tb_nn(v_o, 1587))->q_name), "(\");\n"));
        {
            long long tb_end869 = ((((tb_o_Node *)tb_nn(v_o, 1588))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end869; v_i++) {
                const char * v_sep = "";
                if ((v_i > 0LL)) {
                    v_sep = ", ";
                }
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat("    tb_ps(\"", v_sep), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1592))->q_kids, v_i, sizeof(tb_o_Node *), 1592)), 1592))->q_name), ": \");\n"));
                if (((tb_o_Node *)tb_nn(v_o, 1593))->q_by_cols) {
                    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    tb_show_", tb_itos(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1594))->q_kids, v_i, sizeof(tb_o_Node *), 1594)), 1594))->q_ty)), "(x.c->q_"), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1594))->q_kids, v_i, sizeof(tb_o_Node *), 1594)), 1594))->q_name), "[x.i]);\n"));
                } else {
                    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    tb_show_", tb_itos(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1596))->q_kids, v_i, sizeof(tb_o_Node *), 1596)), 1596))->q_ty)), "(x"), f_emit__field_path(v_o, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1596))->q_kids, v_i, sizeof(tb_o_Node *), 1596)), 1596))->q_name)), ");\n"));
                }
            }
        }
        f_emit__put(v_c, "    tb_ps(\")\");\n");
    }
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_cols_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    f_emit__put(v_c, tb_cat(tb_cat("struct tb_cols_", ((tb_o_Node *)tb_nn(v_o, 1604))->q_name), " {\n    long long len, cap;\n"));
    {
        tb_list * tb_l870 = ((tb_o_Node *)tb_nn(v_o, 1605))->q_kids;
        for (long long tb_i870 = 0; tb_i870 < tb_l870->len; tb_i870++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l870->data)[tb_i870];
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1606))->q_ty)), " *q_"), ((tb_o_Node *)tb_nn(v_f, 1606))->q_name), ";\n"));
        }
    }
    f_emit__put(v_c, "};\n");
}

static void f_emit__emit_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((tb_o_Node *)tb_nn(v_o, 1610))->q_by_cols) {
        f_emit__emit_cols_obj(v_c, v_o);
        return;
    }
    if (f_emit__has_cold(v_o)) {
        f_emit__put(v_c, tb_cat(tb_cat("struct tb_c_", ((tb_o_Node *)tb_nn(v_o, 1615))->q_name), " {\n"));
        {
            tb_list * tb_l871 = ((tb_o_Node *)tb_nn(v_o, 1616))->q_kids;
            for (long long tb_i871 = 0; tb_i871 < tb_l871->len; tb_i871++) {
                tb_o_Node * const v_f = ((tb_o_Node * *)tb_l871->data)[tb_i871];
                if (((tb_o_Node *)tb_nn(v_f, 1617))->q_is_cold) {
                    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1618))->q_ty)), " q_"), ((tb_o_Node *)tb_nn(v_f, 1618))->q_name), ";\n"));
                }
            }
        }
        f_emit__put(v_c, "};\n");
    }
    f_emit__put(v_c, tb_cat(tb_cat("struct tb_o_", ((tb_o_Node *)tb_nn(v_o, 1620))->q_name), " {\n"));
    {
        tb_list * tb_l872 = ((tb_o_Node *)tb_nn(v_o, 1621))->q_kids;
        for (long long tb_i872 = 0; tb_i872 < tb_l872->len; tb_i872++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l872->data)[tb_i872];
            if ((!((tb_o_Node *)tb_nn(v_f, 1622))->q_is_cold)) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1623))->q_ty)), " q_"), ((tb_o_Node *)tb_nn(v_f, 1623))->q_name), ";\n"));
            }
        }
    }
    if (f_emit__has_cold(v_o)) {
        f_emit__put(v_c, tb_cat(tb_cat("    struct tb_c_", ((tb_o_Node *)tb_nn(v_o, 1625))->q_name), " *tb_cold;\n"));
    }
    if (((((tb_o_Node *)tb_nn(v_o, 1626))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "    char empty;\n");
    }
    f_emit__put(v_c, "};\n");
}

static _Bool f_emit__has_cold(tb_o_Node * v_o) {
    {
        tb_list * tb_l873 = ((tb_o_Node *)tb_nn(v_o, 1631))->q_kids;
        for (long long tb_i873 = 0; tb_i873 < tb_l873->len; tb_i873++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l873->data)[tb_i873];
            if (((tb_o_Node *)tb_nn(v_f, 1632))->q_is_cold) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1630, "функция has_cold дошла до конца без return");
    return 0;
}

static const char * f_emit__field_path(tb_o_Node * v_o, const char * v_name) {
    const char * v_r = "";
    {
        tb_list * tb_l874 = ((tb_o_Node *)tb_nn(v_o, 1638))->q_kids;
        for (long long tb_i874 = 0; tb_i874 < tb_l874->len; tb_i874++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l874->data)[tb_i874];
            if (((strcmp(((tb_o_Node *)tb_nn(v_f, 1639))->q_name, v_name) == 0) && ((tb_o_Node *)tb_nn(v_f, 1639))->q_is_cold)) {
                return tb_cat("->tb_cold->q_", v_name);
            }
        }
    }
    return tb_cat("->q_", v_name);
    return v_r;
}

static void f_emit__emit_cols_helpers(tb_o_Comp * v_c, tb_o_Node * v_o) {
    const char * v_nm = ((tb_o_Node *)tb_nn(v_o, 1645))->q_name;
    const char * v_cols = tb_cat("tb_cols_", v_nm);
    const char * v_row = tb_cat("tb_row_", v_nm);
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static ", v_cols), " *tb_cols_new_"), v_nm), "(void) {\n"));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", v_cols), " *l = tb_mem(sizeof("), v_cols), "));\n    l->len = 0; l->cap = 4;\n"));
    {
        tb_list * tb_l875 = ((tb_o_Node *)tb_nn(v_o, 1650))->q_kids;
        for (long long tb_i875 = 0; tb_i875 < tb_l875->len; tb_i875++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l875->data)[tb_i875];
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    l->q_", ((tb_o_Node *)tb_nn(v_f, 1651))->q_name), " = tb_mem(sizeof("), f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1651))->q_ty)), ") * 4);\n"));
        }
    }
    f_emit__put(v_c, "    return l;\n}\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static void tb_colgrow_", v_nm), "("), v_cols), " *l) {\n"));
    f_emit__put(v_c, "    if (l->len < l->cap) return;\n    l->cap *= 2;\n");
    {
        tb_list * tb_l876 = ((tb_o_Node *)tb_nn(v_o, 1655))->q_kids;
        for (long long tb_i876 = 0; tb_i876 < tb_l876->len; tb_i876++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l876->data)[tb_i876];
            const char * v_ft = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1656))->q_ty);
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("    l->q_", ((tb_o_Node *)tb_nn(v_f, 1657))->q_name), " = tb_resize(l->q_"), ((tb_o_Node *)tb_nn(v_f, 1657))->q_name), ", sizeof("), v_ft), ") * (l->cap / 2), sizeof("), v_ft), ") * l->cap);\n"));
        }
    }
    f_emit__put(v_c, "}\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("static ", v_row), " tb_colat_"), v_nm), "("), v_cols), " *l, long long i, int line) {\n"));
    f_emit__put(v_c, tb_cat(tb_cat("    if (TB_RARE((unsigned long long)i >= (unsigned long long)((", v_cols), " *)tb_nn(l, line))->len)) tb_crash(line, \"номер за пределами списка\");\n"));
    f_emit__put(v_c, tb_cat(tb_cat("    return (", v_row), "){l, i};\n}\n"));
    {
        tb_list * tb_l877 = ((tb_o_Node *)tb_nn(v_o, 1662))->q_kids;
        for (long long tb_i877 = 0; tb_i877 < tb_l877->len; tb_i877++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l877->data)[tb_i877];
            const char * v_ft = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1663))->q_ty);
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("static inline ", v_ft), " *tb_colf_"), v_nm), "_"), ((tb_o_Node *)tb_nn(v_f, 1664))->q_name), "("), v_row), " r, int line) {\n"));
            f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
            f_emit__put(v_c, tb_cat(tb_cat("    return &r.c->q_", ((tb_o_Node *)tb_nn(v_f, 1666))->q_name), "[r.i];\n}\n"));
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("static void tb_coladd_", v_nm), "("), v_cols), " *l, "), v_row), " r, int line) {\n"));
    f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
    {
        long long tb_end878 = ((((tb_o_Node *)tb_nn(v_o, 1669))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end878; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1670))->q_kids, v_i, sizeof(tb_o_Node *), 1670)), 1670))->q_ty)), " a"), tb_itos(v_i)), " = r.c->q_"), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1670))->q_kids, v_i, sizeof(tb_o_Node *), 1670)), 1670))->q_name), "[r.i];\n"));
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat("    tb_colgrow_", v_nm), "(tb_nn(l, line));\n"));
    {
        long long tb_end879 = ((((tb_o_Node *)tb_nn(v_o, 1672))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end879; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    l->q_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1673))->q_kids, v_i, sizeof(tb_o_Node *), 1673)), 1673))->q_name), "[l->len] = a"), tb_itos(v_i)), ";\n"));
        }
    }
    f_emit__put(v_c, "    l->len += 1;\n}\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static void tb_colpush_", v_nm), "("), v_cols), " *l"));
    {
        long long tb_end880 = ((((tb_o_Node *)tb_nn(v_o, 1676))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end880; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(", ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1677))->q_kids, v_i, sizeof(tb_o_Node *), 1677)), 1677))->q_ty)), " a"), tb_itos(v_i)));
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat(", int line) {\n    tb_colgrow_", v_nm), "(tb_nn(l, line));\n"));
    {
        long long tb_end881 = ((((tb_o_Node *)tb_nn(v_o, 1679))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end881; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    l->q_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1680))->q_kids, v_i, sizeof(tb_o_Node *), 1680)), 1680))->q_name), "[l->len] = a"), tb_itos(v_i)), ";\n"));
        }
    }
    f_emit__put(v_c, "    l->len += 1;\n}\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("static void tb_colset_", v_nm), "("), v_cols), " *l, long long i, "), v_row), " r, int line) {\n"));
    f_emit__put(v_c, tb_cat(tb_cat("    tb_colat_", v_nm), "(l, i, line);\n"));
    f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
    {
        tb_list * tb_l882 = ((tb_o_Node *)tb_nn(v_o, 1685))->q_kids;
        for (long long tb_i882 = 0; tb_i882 < tb_l882->len; tb_i882++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l882->data)[tb_i882];
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    l->q_", ((tb_o_Node *)tb_nn(v_f, 1686))->q_name), "[i] = r.c->q_"), ((tb_o_Node *)tb_nn(v_f, 1686))->q_name), "[r.i];\n"));
        }
    }
    f_emit__put(v_c, "}\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("static ", v_cols), " *tb_colsfrom_"), v_nm), "(long long n, "), v_row), " *rows) {\n"));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", v_cols), " *l = tb_cols_new_"), v_nm), "();\n"));
    f_emit__put(v_c, tb_cat(tb_cat("    for (long long i = 0; i < n; i++) tb_coladd_", v_nm), "(l, rows[i], 0);\n"));
    f_emit__put(v_c, "    return l;\n}\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static ", v_row), " tb_new_"), v_nm), "("));
    if (((((tb_o_Node *)tb_nn(v_o, 1694))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end883 = ((((tb_o_Node *)tb_nn(v_o, 1696))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end883; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1699))->q_kids, v_i, sizeof(tb_o_Node *), 1699)), 1699))->q_ty), " a"), tb_itos(v_i)));
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(") {\n    ", v_cols), " *l = tb_cols_new_"), v_nm), "();\n"));
    {
        long long tb_end884 = ((((tb_o_Node *)tb_nn(v_o, 1701))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end884; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    l->q_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1702))->q_kids, v_i, sizeof(tb_o_Node *), 1702)), 1702))->q_name), "[0] = a"), tb_itos(v_i)), ";\n"));
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat("    l->len = 1;\n    return (", v_row), "){l, 0};\n}\n"));
}

static void f_emit__emit_constructor(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((tb_o_Node *)tb_nn(v_o, 1706))->q_by_cols) {
        f_emit__emit_cols_helpers(v_c, v_o);
        return;
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static tb_o_", ((tb_o_Node *)tb_nn(v_o, 1709))->q_name), " *tb_new_"), ((tb_o_Node *)tb_nn(v_o, 1709))->q_name), "("));
    if (((((tb_o_Node *)tb_nn(v_o, 1710))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end885 = ((((tb_o_Node *)tb_nn(v_o, 1712))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end885; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1715))->q_kids, v_i, sizeof(tb_o_Node *), 1715)), 1715))->q_ty), " a"), tb_itos(v_i)));
        }
    }
    f_emit__put(v_c, ") {\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    tb_o_", ((tb_o_Node *)tb_nn(v_o, 1717))->q_name), " *o = tb_mem(sizeof(tb_o_"), ((tb_o_Node *)tb_nn(v_o, 1717))->q_name), "));\n"));
    if (f_emit__has_cold(v_o)) {
        f_emit__put(v_c, tb_cat(tb_cat("    o->tb_cold = tb_cold_mem(sizeof(struct tb_c_", ((tb_o_Node *)tb_nn(v_o, 1719))->q_name), "));\n"));
    }
    {
        long long tb_end886 = ((((tb_o_Node *)tb_nn(v_o, 1720))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end886; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    o", f_emit__field_path(v_o, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1721))->q_kids, v_i, sizeof(tb_o_Node *), 1721)), 1721))->q_name)), " = a"), tb_itos(v_i)), ";\n"));
        }
    }
    f_emit__put(v_c, "    return o;\n}\n");
}

static void f_emit__emit_c(tb_o_Comp * v_c, tb_o_Node * v_program) {
    tb_list * v_fresh = tb_list_new(sizeof(const char *));
    { tb_list * tb_v887 = v_fresh; tb_list * *tb_p887 = &(((tb_o_Comp *)tb_nn(v_c, 1726))->q_out); *tb_p887 = tb_v887; }
    { long long tb_v888 = 0LL; long long *tb_p888 = &(((tb_o_Comp *)tb_nn(v_c, 1727))->q_label); *tb_p888 = tb_v888; }
    f_emit__put(v_c, "// Сделано компилятором Tachbit 0.7. Не правь руками: правь файл .tb\n");
    f_emit__put(v_c, f_runtime_text__runtime_text());
    {
        tb_list * tb_l889 = ((tb_o_Node *)tb_nn(v_program, 1732))->q_kids;
        for (long long tb_i889 = 0; tb_i889 < tb_l889->len; tb_i889++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l889->data)[tb_i889];
            if ((({ const char *tb_s890 = ((tb_o_Node *)tb_nn(v_top, 1733))->q_kind; ((unsigned char)tb_s890[0] == 111 && (unsigned char)tb_s890[1] == 98 && (unsigned char)tb_s890[2] == 106 && tb_s890[3] == 0); }) && ((tb_o_Node *)tb_nn(v_top, 1733))->q_by_cols)) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("typedef struct tb_cols_", ((tb_o_Node *)tb_nn(v_top, 1734))->q_name), " tb_cols_"), ((tb_o_Node *)tb_nn(v_top, 1734))->q_name), ";\n"));
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("typedef struct { tb_cols_", ((tb_o_Node *)tb_nn(v_top, 1735))->q_name), " *c; long long i; } tb_row_"), ((tb_o_Node *)tb_nn(v_top, 1735))->q_name), ";\n"));
            } else if (({ const char *tb_s891 = ((tb_o_Node *)tb_nn(v_top, 1736))->q_kind; ((unsigned char)tb_s891[0] == 111 && (unsigned char)tb_s891[1] == 98 && (unsigned char)tb_s891[2] == 106 && tb_s891[3] == 0); })) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("typedef struct tb_o_", ((tb_o_Node *)tb_nn(v_top, 1737))->q_name), " tb_o_"), ((tb_o_Node *)tb_nn(v_top, 1737))->q_name), ";\n"));
            }
        }
    }
    {
        tb_list * tb_l892 = ((tb_o_Node *)tb_nn(v_program, 1738))->q_kids;
        for (long long tb_i892 = 0; tb_i892 < tb_l892->len; tb_i892++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l892->data)[tb_i892];
            if (({ const char *tb_s893 = ((tb_o_Node *)tb_nn(v_top, 1739))->q_kind; ((unsigned char)tb_s893[0] == 111 && (unsigned char)tb_s893[1] == 98 && (unsigned char)tb_s893[2] == 106 && tb_s893[3] == 0); })) {
                f_emit__emit_obj(v_c, v_top);
            }
        }
    }
    {
        tb_list * tb_l894 = ((tb_o_Node *)tb_nn(v_program, 1742))->q_kids;
        for (long long tb_i894 = 0; tb_i894 < tb_l894->len; tb_i894++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l894->data)[tb_i894];
            if ((({ const char *tb_s895 = ((tb_o_Node *)tb_nn(v_top, 1743))->q_kind; ((unsigned char)tb_s895[0] == 102 && (unsigned char)tb_s895[1] == 117 && (unsigned char)tb_s895[2] == 110 && (unsigned char)tb_s895[3] == 99 && tb_s895[4] == 0); }) && ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1743))->q_kids, 2LL, sizeof(tb_o_Node *), 1743)), 1743))->q_kids)->len > 1LL))) {
                f_emit__put(v_c, "typedef struct {");
                {
                    long long tb_end896 = ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1745))->q_kids, 2LL, sizeof(tb_o_Node *), 1745)), 1745))->q_kids)->len - 1LL);
                    for (long long v_j = 0LL; v_j <= tb_end896; v_j++) {
                        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(" ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1746))->q_kids, 2LL, sizeof(tb_o_Node *), 1746)), 1746))->q_kids, v_j, sizeof(tb_o_Node *), 1746)), 1746))->q_ty)), " o"), tb_itos(v_j)), ";"));
                    }
                }
                f_emit__put(v_c, tb_cat(tb_cat(" } tb_r_", f_emit__fname(v_top)), ";\n"));
            }
        }
    }
    {
        tb_list * tb_l897 = ((tb_o_Comp *)tb_nn(v_c, 1748))->q_types;
        for (long long tb_i897 = 0; tb_i897 < tb_l897->len; tb_i897++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l897->data)[tb_i897];
            const char * v_k = ((tb_o_Type *)tb_nn(v_t, 1749))->q_kind;
            if (((({ const char *tb_s898 = v_k; !((unsigned char)tb_s898[0] == 110 && (unsigned char)tb_s898[1] == 111 && (unsigned char)tb_s898[2] == 110 && (unsigned char)tb_s898[3] == 101 && tb_s898[4] == 0); }) && ({ const char *tb_s899 = v_k; !((unsigned char)tb_s899[0] == 109 && (unsigned char)tb_s899[1] == 117 && (unsigned char)tb_s899[2] == 108 && (unsigned char)tb_s899[3] == 116 && (unsigned char)tb_s899[4] == 105 && tb_s899[5] == 0); })) && ({ const char *tb_s900 = v_k; !((unsigned char)tb_s900[0] == 117 && (unsigned char)tb_s900[1] == 110 && (unsigned char)tb_s900[2] == 107 && (unsigned char)tb_s900[3] == 110 && (unsigned char)tb_s900[4] == 111 && (unsigned char)tb_s900[5] == 119 && (unsigned char)tb_s900[6] == 110 && tb_s900[7] == 0); }))) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static void tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1751))->q_id)), "("), f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1751))->q_id)), " x);\n"));
            }
        }
    }
    {
        long long tb_end901 = ((((tb_o_Comp *)tb_nn(v_c, 1752))->q_temp_types)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end901; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static _Thread_local ", f_emit__c_type(v_c, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1753))->q_temp_types, v_i, sizeof(long long), 1753)))), " tb_t"), tb_itos(v_i)), ";   // для  f() or запасное\n"));
        }
    }
    {
        tb_list * tb_l902 = ((tb_o_Node *)tb_nn(v_program, 1754))->q_kids;
        for (long long tb_i902 = 0; tb_i902 < tb_l902->len; tb_i902++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l902->data)[tb_i902];
            if (({ const char *tb_s903 = ((tb_o_Node *)tb_nn(v_top, 1755))->q_kind; ((unsigned char)tb_s903[0] == 111 && (unsigned char)tb_s903[1] == 98 && (unsigned char)tb_s903[2] == 106 && tb_s903[3] == 0); })) {
                f_emit__emit_constructor(v_c, v_top);
            }
        }
    }
    {
        tb_list * tb_l904 = ((tb_o_Node *)tb_nn(v_program, 1757))->q_kids;
        for (long long tb_i904 = 0; tb_i904 < tb_l904->len; tb_i904++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l904->data)[tb_i904];
            if (({ const char *tb_s905 = ((tb_o_Node *)tb_nn(v_top, 1758))->q_kind; ((unsigned char)tb_s905[0] == 102 && (unsigned char)tb_s905[1] == 117 && (unsigned char)tb_s905[2] == 110 && (unsigned char)tb_s905[3] == 99 && tb_s905[4] == 0); })) {
                f_emit__emit_signature(v_c, v_top);
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__put(v_c, "\n");
    {
        tb_list * tb_l906 = ((tb_o_Comp *)tb_nn(v_c, 1762))->q_types;
        for (long long tb_i906 = 0; tb_i906 < tb_l906->len; tb_i906++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l906->data)[tb_i906];
            f_emit__emit_show(v_c, v_t);
        }
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 1765))->q_out;
    tb_list * v_funcs_out = tb_list_new(sizeof(const char *));
    { tb_list * tb_v907 = v_funcs_out; tb_list * *tb_p907 = &(((tb_o_Comp *)tb_nn(v_c, 1767))->q_out); *tb_p907 = tb_v907; }
    {
        tb_list * tb_l908 = ((tb_o_Node *)tb_nn(v_program, 1768))->q_kids;
        for (long long tb_i908 = 0; tb_i908 < tb_l908->len; tb_i908++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l908->data)[tb_i908];
            if (({ const char *tb_s909 = ((tb_o_Node *)tb_nn(v_top, 1769))->q_kind; ((unsigned char)tb_s909[0] == 102 && (unsigned char)tb_s909[1] == 117 && (unsigned char)tb_s909[2] == 110 && (unsigned char)tb_s909[3] == 99 && tb_s909[4] == 0); })) {
                f_emit__emit_func(v_c, v_top);
            }
        }
    }
    { tb_list * tb_v910 = v_saved; tb_list * *tb_p910 = &(((tb_o_Comp *)tb_nn(v_c, 1771))->q_out); *tb_p910 = tb_v910; }
    {
        tb_list * tb_l911 = ((tb_o_Comp *)tb_nn(v_c, 1772))->q_workers;
        for (long long tb_i911 = 0; tb_i911 < tb_l911->len; tb_i911++) {
            const char * const v_w = ((const char * *)tb_l911->data)[tb_i911];
            f_emit__put(v_c, v_w);
        }
    }
    {
        tb_list * tb_l912 = v_funcs_out;
        for (long long tb_i912 = 0; tb_i912 < tb_l912->len; tb_i912++) {
            const char * const v_f = ((const char * *)tb_l912->data)[tb_i912];
            f_emit__put(v_c, v_f);
        }
    }
    tb_o_Node * v_main_func = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1776))->q_kids, 0LL, sizeof(tb_o_Node *), 1776));
    {
        tb_list * tb_l913 = ((tb_o_Node *)tb_nn(v_program, 1777))->q_kids;
        for (long long tb_i913 = 0; tb_i913 < tb_l913->len; tb_i913++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l913->data)[tb_i913];
            if (((({ const char *tb_s914 = ((tb_o_Node *)tb_nn(v_top, 1778))->q_kind; ((unsigned char)tb_s914[0] == 102 && (unsigned char)tb_s914[1] == 117 && (unsigned char)tb_s914[2] == 110 && (unsigned char)tb_s914[3] == 99 && tb_s914[4] == 0); }) && ({ const char *tb_s915 = ((tb_o_Node *)tb_nn(v_top, 1778))->q_name; ((unsigned char)tb_s915[0] == 109 && (unsigned char)tb_s915[1] == 97 && (unsigned char)tb_s915[2] == 105 && (unsigned char)tb_s915[3] == 110 && tb_s915[4] == 0); })) && (strcmp(((tb_o_Node *)tb_nn(v_top, 1778))->q_mod, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1778))->q_kids, 0LL, sizeof(tb_o_Node *), 1778)), 1778))->q_mod) == 0))) {
                v_main_func = v_top;
            }
        }
    }
    f_emit__put(v_c, "\nint main(int argc, char **argv) {\n");
    f_emit__put(v_c, "    tb_args = tb_list_new(sizeof(const char *));\n");
    f_emit__put(v_c, "    for (int i = 1; i < argc; i++) *(const char **)tb_grow(tb_args) = argv[i];\n");
    f_emit__put(v_c, tb_cat(tb_cat("    ", f_emit__fname(v_main_func)), "();\n    return 0;\n}\n"));
}

static const char * f_runtime_text__runtime_text(void) {
    tb_list * v_lines = tb_list_new(sizeof(const char *));
    { const char * tb_v916 = "// Библиотека, которую Tachbit кладёт в начало каждой программы на C.\n"; *(const char * *)tb_grow(v_lines) = tb_v916; }
    { const char * tb_v917 = "// Это обычный C. Компилятор на Tachbit встраивает этот текст через runtime_text.tb,\n"; *(const char * *)tb_grow(v_lines) = tb_v917; }
    { const char * tb_v918 = "// который делает tools/embed_runtime.py. Поменял этот файл — запусти:\n"; *(const char * *)tb_grow(v_lines) = tb_v918; }
    { const char * tb_v919 = "//     python3 tools/embed_runtime.py\n"; *(const char * *)tb_grow(v_lines) = tb_v919; }
    { const char * tb_v920 = "#include <stdio.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v920; }
    { const char * tb_v921 = "#include <stdlib.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v921; }
    { const char * tb_v922 = "#include <string.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v922; }
    { const char * tb_v923 = "#include <math.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v923; }
    { const char * tb_v924 = "#include <pthread.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v924; }
    { const char * tb_v925 = "#include <unistd.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v925; }
    { const char * tb_v926 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v926; }
    { const char * tb_v927 = "// подсказки для clang / gcc: ошибка бывает редко, а tb_crash не возвращается\n"; *(const char * *)tb_grow(v_lines) = tb_v927; }
    { const char * tb_v928 = "#if defined(__GNUC__)\n"; *(const char * *)tb_grow(v_lines) = tb_v928; }
    { const char * tb_v929 = "#define TB_RARE(x) __builtin_expect(!!(x), 0)\n"; *(const char * *)tb_grow(v_lines) = tb_v929; }
    { const char * tb_v930 = "#define TB_COLD __attribute__((cold, noreturn, noinline))\n"; *(const char * *)tb_grow(v_lines) = tb_v930; }
    { const char * tb_v931 = "#else\n"; *(const char * *)tb_grow(v_lines) = tb_v931; }
    { const char * tb_v932 = "#define TB_RARE(x) (x)\n"; *(const char * *)tb_grow(v_lines) = tb_v932; }
    { const char * tb_v933 = "#define TB_COLD\n"; *(const char * *)tb_grow(v_lines) = tb_v933; }
    { const char * tb_v934 = "#endif\n"; *(const char * *)tb_grow(v_lines) = tb_v934; }
    { const char * tb_v935 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v935; }
    { const char * tb_v936 = "// список: кусок памяти, len элементов по size байт, место на cap\n"; *(const char * *)tb_grow(v_lines) = tb_v936; }
    { const char * tb_v937 = "typedef struct { char *data; long long len; long long cap; long long size; } tb_list;\n"; *(const char * *)tb_grow(v_lines) = tb_v937; }
    { const char * tb_v938 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v938; }
    { const char * tb_v939 = "TB_COLD static void tb_crash(int line, const char *msg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v939; }
    { const char * tb_v940 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v940; }
    { const char * tb_v941 = "    // в терминале — красным (если не задан NO_COLOR)\n"; *(const char * *)tb_grow(v_lines) = tb_v941; }
    { const char * tb_v942 = "    int color = isatty(2) && getenv(\"NO_COLOR\") == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v942; }
    { const char * tb_v943 = "    if (color) fputs(\"\\033[31m\", stderr);\n"; *(const char * *)tb_grow(v_lines) = tb_v943; }
    { const char * tb_v944 = "    if (line > 0) fprintf(stderr, \"строка %d: %s\", line, msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v944; }
    { const char * tb_v945 = "    else fprintf(stderr, \"%s\", msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v945; }
    { const char * tb_v946 = "    fputs(color ? \"\\033[0m\\n\" : \"\\n\", stderr);\n"; *(const char * *)tb_grow(v_lines) = tb_v946; }
    { const char * tb_v947 = "    exit(1);\n"; *(const char * *)tb_grow(v_lines) = tb_v947; }
    { const char * tb_v948 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v948; }
    { const char * tb_v949 = "// ---------- temp: временная память ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v949; }
    { const char * tb_v950 = "// Внутри блока temp память берётся из больших кусков подряд (быстро, без malloc),\n"; *(const char * *)tb_grow(v_lines) = tb_v950; }
    { const char * tb_v951 = "// а в конце блока все куски отдаются разом. У каждого ядра свои куски.\n"; *(const char * *)tb_grow(v_lines) = tb_v951; }
    { const char * tb_v952 = "typedef struct { char *mem; long long size, used; } tb_chunk;\n"; *(const char * *)tb_grow(v_lines) = tb_v952; }
    { const char * tb_v953 = "typedef struct { int n; long long used; } tb_mark;\n"; *(const char * *)tb_grow(v_lines) = tb_v953; }
    { const char * tb_v954 = "static _Thread_local tb_chunk *tb_chunks = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v954; }
    { const char * tb_v955 = "static _Thread_local int tb_nchunks = 0, tb_capchunks = 0, tb_temp_depth = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v955; }
    { const char * tb_v956 = "static int tb_in_temp(const void *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v956; }
    { const char * tb_v957 = "    for (int i = tb_nchunks - 1; i >= 0; i--) {\n"; *(const char * *)tb_grow(v_lines) = tb_v957; }
    { const char * tb_v958 = "        if ((const char *)p >= tb_chunks[i].mem && (const char *)p < tb_chunks[i].mem + tb_chunks[i].size) return 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v958; }
    { const char * tb_v959 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v959; }
    { const char * tb_v960 = "    return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v960; }
    { const char * tb_v961 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v961; }
    { const char * tb_v962 = "static void *tb_temp_alloc(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v962; }
    { const char * tb_v963 = "    n = n > 0 ? (n + 15) & ~15LL : 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v963; }
    { const char * tb_v964 = "    tb_chunk *c = tb_nchunks > 0 ? &tb_chunks[tb_nchunks - 1] : 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v964; }
    { const char * tb_v965 = "    if (c == 0 || c->used + n > c->size) {\n"; *(const char * *)tb_grow(v_lines) = tb_v965; }
    { const char * tb_v966 = "        long long size = c != 0 ? c->size * 2 : (1LL << 20);\n"; *(const char * *)tb_grow(v_lines) = tb_v966; }
    { const char * tb_v967 = "        if (size < n) size = n;\n"; *(const char * *)tb_grow(v_lines) = tb_v967; }
    { const char * tb_v968 = "        if (tb_nchunks == tb_capchunks) {\n"; *(const char * *)tb_grow(v_lines) = tb_v968; }
    { const char * tb_v969 = "            tb_capchunks = tb_capchunks * 2 + 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v969; }
    { const char * tb_v970 = "            tb_chunks = realloc(tb_chunks, sizeof(tb_chunk) * tb_capchunks);\n"; *(const char * *)tb_grow(v_lines) = tb_v970; }
    { const char * tb_v971 = "            if (tb_chunks == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v971; }
    { const char * tb_v972 = "        }\n"; *(const char * *)tb_grow(v_lines) = tb_v972; }
    { const char * tb_v973 = "        char *mem = malloc(size);\n"; *(const char * *)tb_grow(v_lines) = tb_v973; }
    { const char * tb_v974 = "        if (mem == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v974; }
    { const char * tb_v975 = "        tb_chunks[tb_nchunks] = (tb_chunk){mem, size, 0};\n"; *(const char * *)tb_grow(v_lines) = tb_v975; }
    { const char * tb_v976 = "        c = &tb_chunks[tb_nchunks];\n"; *(const char * *)tb_grow(v_lines) = tb_v976; }
    { const char * tb_v977 = "        tb_nchunks += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v977; }
    { const char * tb_v978 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v978; }
    { const char * tb_v979 = "    void *p = c->mem + c->used;\n"; *(const char * *)tb_grow(v_lines) = tb_v979; }
    { const char * tb_v980 = "    c->used += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v980; }
    { const char * tb_v981 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v981; }
    { const char * tb_v982 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v982; }
    { const char * tb_v983 = "static tb_mark tb_temp_begin(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v983; }
    { const char * tb_v984 = "    tb_temp_depth += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v984; }
    { const char * tb_v985 = "    return (tb_mark){tb_nchunks, tb_nchunks > 0 ? tb_chunks[tb_nchunks - 1].used : 0};\n"; *(const char * *)tb_grow(v_lines) = tb_v985; }
    { const char * tb_v986 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v986; }
    { const char * tb_v987 = "// длина последних текстов: s[i] в цикле не пересчитывает strlen каждый раз\n"; *(const char * *)tb_grow(v_lines) = tb_v987; }
    { const char * tb_v988 = "static _Thread_local const char *tb_lc_p[2];\n"; *(const char * *)tb_grow(v_lines) = tb_v988; }
    { const char * tb_v989 = "static _Thread_local long long tb_lc_n[2];\n"; *(const char * *)tb_grow(v_lines) = tb_v989; }
    { const char * tb_v990 = "static _Thread_local int tb_lc_next;\n"; *(const char * *)tb_grow(v_lines) = tb_v990; }
    { const char * tb_v991 = "static inline void tb_lc_clear(void) { tb_lc_p[0] = 0; tb_lc_p[1] = 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v991; }
    { const char * tb_v992 = "static void tb_temp_end(tb_mark m) {\n"; *(const char * *)tb_grow(v_lines) = tb_v992; }
    { const char * tb_v993 = "    // первый кусок оставляем пустым: следующий temp возьмёт его без malloc\n"; *(const char * *)tb_grow(v_lines) = tb_v993; }
    { const char * tb_v994 = "    while (tb_nchunks > m.n && tb_nchunks > 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v994; }
    { const char * tb_v995 = "        tb_nchunks -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v995; }
    { const char * tb_v996 = "        free(tb_chunks[tb_nchunks].mem);\n"; *(const char * *)tb_grow(v_lines) = tb_v996; }
    { const char * tb_v997 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v997; }
    { const char * tb_v998 = "    if (tb_nchunks > m.n) tb_chunks[0].used = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v998; }
    { const char * tb_v999 = "    else if (tb_nchunks > 0) tb_chunks[tb_nchunks - 1].used = m.used;\n"; *(const char * *)tb_grow(v_lines) = tb_v999; }
    { const char * tb_v1000 = "    tb_temp_depth -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1000; }
    { const char * tb_v1001 = "    tb_lc_clear();\n"; *(const char * *)tb_grow(v_lines) = tb_v1001; }
    { const char * tb_v1002 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1002; }
    { const char * tb_v1003 = "static void *tb_mem(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1003; }
    { const char * tb_v1004 = "    if (tb_temp_depth > 0) return tb_temp_alloc(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1004; }
    { const char * tb_v1005 = "    void *p = malloc(n > 0 ? n : 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1005; }
    { const char * tb_v1006 = "    if (p == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1006; }
    { const char * tb_v1007 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1007; }
    { const char * tb_v1008 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1008; }
    { const char * tb_v1009 = "// вырасти кусок: временный растёт во временной памяти, обычный — через realloc\n"; *(const char * *)tb_grow(v_lines) = tb_v1009; }
    { const char * tb_v1010 = "static void *tb_resize(void *p, long long old_n, long long new_n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1010; }
    { const char * tb_v1011 = "    tb_lc_clear();\n"; *(const char * *)tb_grow(v_lines) = tb_v1011; }
    { const char * tb_v1012 = "    if (tb_nchunks > 0 && tb_in_temp(p)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1012; }
    { const char * tb_v1013 = "        void *q = tb_temp_alloc(new_n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1013; }
    { const char * tb_v1014 = "        memcpy(q, p, old_n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1014; }
    { const char * tb_v1015 = "        return q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1015; }
    { const char * tb_v1016 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1016; }
    { const char * tb_v1017 = "    p = realloc(p, new_n > 0 ? new_n : 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1017; }
    { const char * tb_v1018 = "    if (p == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1018; }
    { const char * tb_v1019 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1019; }
    { const char * tb_v1020 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1020; }
    { const char * tb_v1021 = "static tb_list *tb_list_new(long long size) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1021; }
    { const char * tb_v1022 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1022; }
    { const char * tb_v1023 = "    l->len = 0; l->cap = 4; l->size = size; l->data = tb_mem(size * 4);\n"; *(const char * *)tb_grow(v_lines) = tb_v1023; }
    { const char * tb_v1024 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1024; }
    { const char * tb_v1025 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1025; }
    { const char * tb_v1026 = "static tb_list *tb_list_from(long long n, long long size, const void *src) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1026; }
    { const char * tb_v1027 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1027; }
    { const char * tb_v1028 = "    l->len = n; l->cap = n; l->size = size; l->data = tb_mem(size * n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1028; }
    { const char * tb_v1029 = "    memcpy(l->data, src, size * n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1029; }
    { const char * tb_v1030 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1030; }
    { const char * tb_v1031 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1031; }
    { const char * tb_v1032 = "// size передаётся как sizeof(T): после встраивания C знает его заранее\n"; *(const char * *)tb_grow(v_lines) = tb_v1032; }
    { const char * tb_v1033 = "static inline void *tb_at(tb_list *l, long long i, long long size, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1033; }
    { const char * tb_v1034 = "    // одно сравнение вместо двух: отрицательный номер как unsigned становится огромным\n"; *(const char * *)tb_grow(v_lines) = tb_v1034; }
    { const char * tb_v1035 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1035; }
    { const char * tb_v1036 = "    return l->data + i * size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1036; }
    { const char * tb_v1037 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1037; }
    { const char * tb_v1038 = "static inline void *tb_grow(tb_list *l) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1038; }
    { const char * tb_v1039 = "    // cap < 0: это кусок (each piece in data by N) или список, который сейчас режут на куски\n"; *(const char * *)tb_grow(v_lines) = tb_v1039; }
    { const char * tb_v1040 = "    if (TB_RARE(l->len >= l->cap)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1040; }
    { const char * tb_v1041 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1041; }
    { const char * tb_v1042 = "        long long old = l->cap * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1042; }
    { const char * tb_v1043 = "        l->cap = l->cap * 2 + 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v1043; }
    { const char * tb_v1044 = "        // realloc часто растит большой кусок на месте, без копирования\n"; *(const char * *)tb_grow(v_lines) = tb_v1044; }
    { const char * tb_v1045 = "        l->data = tb_resize(l->data, old, l->cap * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1045; }
    { const char * tb_v1046 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1046; }
    { const char * tb_v1047 = "    l->len += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1047; }
    { const char * tb_v1048 = "    return l->data + (l->len - 1) * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1048; }
    { const char * tb_v1049 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1049; }
    { const char * tb_v1050 = "static void *tb_pop(tb_list *l, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1050; }
    { const char * tb_v1051 = "    if (l->len == 0) tb_crash(line, \".pop() у пустого списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1051; }
    { const char * tb_v1052 = "    l->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1052; }
    { const char * tb_v1053 = "    return l->data + l->len * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1053; }
    { const char * tb_v1054 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1054; }
    { const char * tb_v1055 = "// ---------- list bool: по одному биту на элемент (в 8 раз меньше памяти) ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1055; }
    { const char * tb_v1056 = "// тот же tb_list: len и cap — в битах, size = 0, data — слова по 64 бита\n"; *(const char * *)tb_grow(v_lines) = tb_v1056; }
    { const char * tb_v1057 = "static tb_list *tb_bits_new(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1057; }
    { const char * tb_v1058 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1058; }
    { const char * tb_v1059 = "    l->len = 0; l->cap = 64; l->size = 0; l->data = tb_mem(8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1059; }
    { const char * tb_v1060 = "    memset(l->data, 0, 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1060; }
    { const char * tb_v1061 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1061; }
    { const char * tb_v1062 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1062; }
    { const char * tb_v1063 = "static tb_list *tb_bits_from(long long n, const unsigned char *src) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1063; }
    { const char * tb_v1064 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1064; }
    { const char * tb_v1065 = "    long long words = (n + 63) / 64 + (n == 0);\n"; *(const char * *)tb_grow(v_lines) = tb_v1065; }
    { const char * tb_v1066 = "    l->len = n; l->cap = words * 64; l->size = 0; l->data = tb_mem(words * 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1066; }
    { const char * tb_v1067 = "    memset(l->data, 0, words * 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1067; }
    { const char * tb_v1068 = "    unsigned long long *w = (unsigned long long *)l->data;\n"; *(const char * *)tb_grow(v_lines) = tb_v1068; }
    { const char * tb_v1069 = "    for (long long i = 0; i < n; i++) if (src[i]) w[i >> 6] |= 1ULL << (i & 63);\n"; *(const char * *)tb_grow(v_lines) = tb_v1069; }
    { const char * tb_v1070 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1070; }
    { const char * tb_v1071 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1071; }
    { const char * tb_v1072 = "// бит без проверки номера (для each — номер всегда внутри)\n"; *(const char * *)tb_grow(v_lines) = tb_v1072; }
    { const char * tb_v1073 = "#define TB_BIT(l, i) ((_Bool)((((unsigned long long *)(l)->data)[(i) >> 6] >> ((i) & 63)) & 1))\n"; *(const char * *)tb_grow(v_lines) = tb_v1073; }
    { const char * tb_v1074 = "static inline _Bool tb_bget(tb_list *l, long long i, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1074; }
    { const char * tb_v1075 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1075; }
    { const char * tb_v1076 = "    return TB_BIT(l, i);\n"; *(const char * *)tb_grow(v_lines) = tb_v1076; }
    { const char * tb_v1077 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1077; }
    { const char * tb_v1078 = "static inline void tb_bset(tb_list *l, long long i, _Bool v, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1078; }
    { const char * tb_v1079 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1079; }
    { const char * tb_v1080 = "    unsigned long long *w = (unsigned long long *)l->data + (i >> 6), m = 1ULL << (i & 63);\n"; *(const char * *)tb_grow(v_lines) = tb_v1080; }
    { const char * tb_v1081 = "    *w = v ? (*w | m) : (*w & ~m);\n"; *(const char * *)tb_grow(v_lines) = tb_v1081; }
    { const char * tb_v1082 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1082; }
    { const char * tb_v1083 = "static inline void tb_badd(tb_list *l, _Bool v) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1083; }
    { const char * tb_v1084 = "    if (TB_RARE(l->len >= l->cap)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1084; }
    { const char * tb_v1085 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1085; }
    { const char * tb_v1086 = "        long long old = l->cap / 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1086; }
    { const char * tb_v1087 = "        l->cap = l->cap * 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1087; }
    { const char * tb_v1088 = "        l->data = tb_resize(l->data, old, l->cap / 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1088; }
    { const char * tb_v1089 = "        memset(l->data + old, 0, l->cap / 8 - old);\n"; *(const char * *)tb_grow(v_lines) = tb_v1089; }
    { const char * tb_v1090 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1090; }
    { const char * tb_v1091 = "    long long i = l->len++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1091; }
    { const char * tb_v1092 = "    unsigned long long *w = (unsigned long long *)l->data + (i >> 6), m = 1ULL << (i & 63);\n"; *(const char * *)tb_grow(v_lines) = tb_v1092; }
    { const char * tb_v1093 = "    *w = v ? (*w | m) : (*w & ~m);\n"; *(const char * *)tb_grow(v_lines) = tb_v1093; }
    { const char * tb_v1094 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1094; }
    { const char * tb_v1095 = "static inline _Bool tb_bpop(tb_list *l, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1095; }
    { const char * tb_v1096 = "    if (l->len == 0) tb_crash(line, \".pop() у пустого списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1096; }
    { const char * tb_v1097 = "    l->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1097; }
    { const char * tb_v1098 = "    return TB_BIT(l, l->len);\n"; *(const char * *)tb_grow(v_lines) = tb_v1098; }
    { const char * tb_v1099 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1099; }
    { const char * tb_v1100 = "// N одинаковых битов в конец (repeat N times: flags.add(true))\n"; *(const char * *)tb_grow(v_lines) = tb_v1100; }
    { const char * tb_v1101 = "static void tb_bfill(tb_list *l, long long n, _Bool v) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1101; }
    { const char * tb_v1102 = "    if (n <= 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1102; }
    { const char * tb_v1103 = "    if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1103; }
    { const char * tb_v1104 = "    long long need = l->len + n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1104; }
    { const char * tb_v1105 = "    if (need > l->cap) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1105; }
    { const char * tb_v1106 = "        long long old = l->cap / 8, cap = l->cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1106; }
    { const char * tb_v1107 = "        while (cap < need) cap *= 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1107; }
    { const char * tb_v1108 = "        l->data = tb_resize(l->data, old, cap / 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1108; }
    { const char * tb_v1109 = "        memset(l->data + old, 0, cap / 8 - old);\n"; *(const char * *)tb_grow(v_lines) = tb_v1109; }
    { const char * tb_v1110 = "        l->cap = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1110; }
    { const char * tb_v1111 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1111; }
    { const char * tb_v1112 = "    unsigned long long *w = (unsigned long long *)l->data;\n"; *(const char * *)tb_grow(v_lines) = tb_v1112; }
    { const char * tb_v1113 = "    long long i = l->len;\n"; *(const char * *)tb_grow(v_lines) = tb_v1113; }
    { const char * tb_v1114 = "    for (; i < need && (i & 63); i++) w[i >> 6] = v ? (w[i >> 6] | 1ULL << (i & 63)) : (w[i >> 6] & ~(1ULL << (i & 63)));\n"; *(const char * *)tb_grow(v_lines) = tb_v1114; }
    { const char * tb_v1115 = "    if (need - i >= 64) { memset(w + (i >> 6), v ? 0xff : 0, ((need - i) >> 6) * 8); i += (need - i) & ~63LL; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1115; }
    { const char * tb_v1116 = "    for (; i < need; i++) w[i >> 6] = v ? (w[i >> 6] | 1ULL << (i & 63)) : (w[i >> 6] & ~(1ULL << (i & 63)));\n"; *(const char * *)tb_grow(v_lines) = tb_v1116; }
    { const char * tb_v1117 = "    l->len = need;\n"; *(const char * *)tb_grow(v_lines) = tb_v1117; }
    { const char * tb_v1118 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1118; }
    { const char * tb_v1119 = "// значения dict bool лежат байтами — в list bool их надо переложить в биты\n"; *(const char * *)tb_grow(v_lines) = tb_v1119; }
    { const char * tb_v1120 = "static tb_list *tb_bits_of_bytes(tb_list *b) { return tb_bits_from(b->len, (const unsigned char *)b->data); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1120; }
    { const char * tb_v1121 = "// N одинаковых элементов в конец (repeat N times: nums.add(0))\n"; *(const char * *)tb_grow(v_lines) = tb_v1121; }
    { const char * tb_v1122 = "static void tb_fill(tb_list *l, long long n, const void *v) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1122; }
    { const char * tb_v1123 = "    if (n <= 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1123; }
    { const char * tb_v1124 = "    if (l->len + n > l->cap) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1124; }
    { const char * tb_v1125 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1125; }
    { const char * tb_v1126 = "        long long cap = l->cap * 2 + 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v1126; }
    { const char * tb_v1127 = "        if (cap < l->len + n) cap = l->len + n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1127; }
    { const char * tb_v1128 = "        l->data = tb_resize(l->data, l->cap * l->size, cap * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1128; }
    { const char * tb_v1129 = "        l->cap = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1129; }
    { const char * tb_v1130 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1130; }
    { const char * tb_v1131 = "    char *p = l->data + l->len * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1131; }
    { const char * tb_v1132 = "    int zero = 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1132; }
    { const char * tb_v1133 = "    for (long long b = 0; b < l->size; b++) if (((const char *)v)[b]) zero = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1133; }
    { const char * tb_v1134 = "    if (zero) memset(p, 0, n * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1134; }
    { const char * tb_v1135 = "    else for (long long i = 0; i < n; i++) memcpy(p + i * l->size, v, l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1135; }
    { const char * tb_v1136 = "    l->len += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1136; }
    { const char * tb_v1137 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1137; }
    { const char * tb_v1138 = "static void *tb_nn(void *p, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1138; }
    { const char * tb_v1139 = "    if (p == 0) tb_crash(line, \"obj пустой: его ещё не создали\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1139; }
    { const char * tb_v1140 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1140; }
    { const char * tb_v1141 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1141; }
    { const char * tb_v1142 = "static const char *tb_cat(const char *a, const char *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1142; }
    { const char * tb_v1143 = "    size_t la = strlen(a), lb = strlen(b);\n"; *(const char * *)tb_grow(v_lines) = tb_v1143; }
    { const char * tb_v1144 = "    char *s = tb_mem(la + lb + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1144; }
    { const char * tb_v1145 = "    memcpy(s, a, la);\n"; *(const char * *)tb_grow(v_lines) = tb_v1145; }
    { const char * tb_v1146 = "    memcpy(s + la, b, lb + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1146; }
    { const char * tb_v1147 = "    return s;\n"; *(const char * *)tb_grow(v_lines) = tb_v1147; }
    { const char * tb_v1148 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1148; }
    { const char * tb_v1149 = "static const char *tb_itos(long long x) { char *s = tb_mem(32); snprintf(s, 32, \"%lld\", x); return s; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1149; }
    { const char * tb_v1150 = "static const char *tb_dtos(double x) { char *s = tb_mem(40); snprintf(s, 40, \"%g\", x); return s; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1150; }
    { const char * tb_v1151 = "static const char *tb_btos(int x) { return x ? \"true\" : \"false\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1151; }
    { const char * tb_v1152 = "// текст из одного байта: 256 готовых строк, без выделения памяти на каждую букву\n"; *(const char * *)tb_grow(v_lines) = tb_v1152; }
    { const char * tb_v1153 = "static char tb_chars[512];\n"; *(const char * *)tb_grow(v_lines) = tb_v1153; }
    { const char * tb_v1154 = "__attribute__((constructor)) static void tb_chars_init(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1154; }
    { const char * tb_v1155 = "    for (int i = 0; i < 256; i++) { tb_chars[2 * i] = (char)i; tb_chars[2 * i + 1] = 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1155; }
    { const char * tb_v1156 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1156; }
    { const char * tb_v1157 = "static inline const char *tb_chr(long long c) { return tb_chars + 2 * (unsigned char)c; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1157; }
    { const char * tb_v1158 = "static inline long long tb_tlen(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1158; }
    { const char * tb_v1159 = "    if (tb_lc_p[0] == s) return tb_lc_n[0];\n"; *(const char * *)tb_grow(v_lines) = tb_v1159; }
    { const char * tb_v1160 = "    if (tb_lc_p[1] == s) return tb_lc_n[1];\n"; *(const char * *)tb_grow(v_lines) = tb_v1160; }
    { const char * tb_v1161 = "    long long n = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1161; }
    { const char * tb_v1162 = "    tb_lc_p[tb_lc_next] = s; tb_lc_n[tb_lc_next] = n; tb_lc_next ^= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1162; }
    { const char * tb_v1163 = "    return n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1163; }
    { const char * tb_v1164 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1164; }
    { const char * tb_v1165 = "// n — длина, если компилятор её знает (текст, в который дописывают), иначе -1\n"; *(const char * *)tb_grow(v_lines) = tb_v1165; }
    { const char * tb_v1166 = "static inline const char *tb_char_at(const char *s, long long i, long long n, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1166; }
    { const char * tb_v1167 = "    if (n < 0) n = tb_tlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1167; }
    { const char * tb_v1168 = "    if (i < 0 || i >= n) tb_crash(line, \"номер за пределами текста\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1168; }
    { const char * tb_v1169 = "    return tb_chr((unsigned char)s[i]);\n"; *(const char * *)tb_grow(v_lines) = tb_v1169; }
    { const char * tb_v1170 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1170; }
    { const char * tb_v1171 = "// s += b: дописать в конец своей памяти s, а не собирать третий текст из двух.\n"; *(const char * *)tb_grow(v_lines) = tb_v1171; }
    { const char * tb_v1172 = "// *rl — длина s в своей памяти (-1: своей памяти нет, s могли сохранить где-то ещё — сначала копия),\n"; *(const char * *)tb_grow(v_lines) = tb_v1172; }
    { const char * tb_v1173 = "// *rc — сколько там места.\n"; *(const char * *)tb_grow(v_lines) = tb_v1173; }
    { const char * tb_v1174 = "static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1174; }
    { const char * tb_v1175 = "static inline void tb_app(const char **p, long long *rl, long long *rc, const char *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1175; }
    { const char * tb_v1176 = "    tb_app_n(p, rl, rc, b, (long long)strlen(b));\n"; *(const char * *)tb_grow(v_lines) = tb_v1176; }
    { const char * tb_v1177 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1177; }
    { const char * tb_v1178 = "// дописать число: цифры пишутся прямо в конец, без отдельного текста\n"; *(const char * *)tb_grow(v_lines) = tb_v1178; }
    { const char * tb_v1179 = "static inline void tb_app_i(const char **p, long long *rl, long long *rc, long long x) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1179; }
    { const char * tb_v1180 = "    char d[24]; int n = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1180; }
    { const char * tb_v1181 = "    unsigned long long u = x < 0 ? 0ULL - (unsigned long long)x : (unsigned long long)x;\n"; *(const char * *)tb_grow(v_lines) = tb_v1181; }
    { const char * tb_v1182 = "    do { d[23 - n++] = (char)('0' + u % 10); u /= 10; } while (u);\n"; *(const char * *)tb_grow(v_lines) = tb_v1182; }
    { const char * tb_v1183 = "    if (x < 0) d[23 - n++] = '-';\n"; *(const char * *)tb_grow(v_lines) = tb_v1183; }
    { const char * tb_v1184 = "    tb_app_n(p, rl, rc, d + 24 - n, n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1184; }
    { const char * tb_v1185 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1185; }
    { const char * tb_v1186 = "static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1186; }
    { const char * tb_v1187 = "// ядро отдаёт свой кусок в общий текст: если общий ещё пустой — забирает память куска целиком, без копии\n"; *(const char * *)tb_grow(v_lines) = tb_v1187; }
    { const char * tb_v1188 = "static void tb_app_take(const char **p, long long *rl, long long *rc, const char *b, long long brl, long long brc) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1188; }
    { const char * tb_v1189 = "    if (*rl < 0 && (*p)[0] == 0 && brl >= 0) { *p = b; *rl = brl; *rc = brc; return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1189; }
    { const char * tb_v1190 = "    tb_app_n(p, rl, rc, b, brl >= 0 ? brl : (long long)strlen(b));\n"; *(const char * *)tb_grow(v_lines) = tb_v1190; }
    { const char * tb_v1191 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1191; }
    { const char * tb_v1192 = "static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1192; }
    { const char * tb_v1193 = "    char *s = (char *)*p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1193; }
    { const char * tb_v1194 = "    if (*rl < 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1194; }
    { const char * tb_v1195 = "        long long la = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1195; }
    { const char * tb_v1196 = "        long long cap = 2 * (la + lb) + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1196; }
    { const char * tb_v1197 = "        if (cap < 64) cap = 64;\n"; *(const char * *)tb_grow(v_lines) = tb_v1197; }
    { const char * tb_v1198 = "        char *r = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1198; }
    { const char * tb_v1199 = "        memcpy(r, s, la);\n"; *(const char * *)tb_grow(v_lines) = tb_v1199; }
    { const char * tb_v1200 = "        memcpy(r + la, b, lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1200; }
    { const char * tb_v1201 = "        r[la + lb] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1201; }
    { const char * tb_v1202 = "        *p = r; *rl = la + lb; *rc = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1202; }
    { const char * tb_v1203 = "        return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1203; }
    { const char * tb_v1204 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1204; }
    { const char * tb_v1205 = "    long long need = *rl + lb + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1205; }
    { const char * tb_v1206 = "    if (need > *rc) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1206; }
    { const char * tb_v1207 = "        long long cap = 2 * need;\n"; *(const char * *)tb_grow(v_lines) = tb_v1207; }
    { const char * tb_v1208 = "        long long off = (b >= s && b < s + *rc) ? b - s : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1208; }
    { const char * tb_v1209 = "        s = tb_resize(s, *rl + 1, cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1209; }
    { const char * tb_v1210 = "        if (off >= 0) b = s + off;\n"; *(const char * *)tb_grow(v_lines) = tb_v1210; }
    { const char * tb_v1211 = "        *p = s; *rc = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1211; }
    { const char * tb_v1212 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1212; }
    { const char * tb_v1213 = "    memmove(s + *rl, b, lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1213; }
    { const char * tb_v1214 = "    *rl += lb;\n"; *(const char * *)tb_grow(v_lines) = tb_v1214; }
    { const char * tb_v1215 = "    s[*rl] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1215; }
    { const char * tb_v1216 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1216; }
    { const char * tb_v1217 = "static const char *tb_cut(const char *s, long long from, long long to, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1217; }
    { const char * tb_v1218 = "    long long n = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1218; }
    { const char * tb_v1219 = "    if (from < 0 || to >= n || from > to + 1) tb_crash(line, \"cut: номера за пределами текста\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1219; }
    { const char * tb_v1220 = "    char *r = tb_mem(to - from + 2);\n"; *(const char * *)tb_grow(v_lines) = tb_v1220; }
    { const char * tb_v1221 = "    memcpy(r, s + from, to - from + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1221; }
    { const char * tb_v1222 = "    r[to - from + 1] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1222; }
    { const char * tb_v1223 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1223; }
    { const char * tb_v1224 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1224; }
    { const char * tb_v1225 = "static long long tb_code(const char *s, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1225; }
    { const char * tb_v1226 = "    if (s[0] == 0) tb_crash(line, \"code: текст пустой\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1226; }
    { const char * tb_v1227 = "    return (unsigned char)s[0];\n"; *(const char * *)tb_grow(v_lines) = tb_v1227; }
    { const char * tb_v1228 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1228; }
    { const char * tb_v1229 = "static long long tb_div(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"деление на ноль\"); return a / b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1229; }
    { const char * tb_v1230 = "static long long tb_mod(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"остаток от деления на ноль\"); return a % b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1230; }
    { const char * tb_v1231 = "static long long tb_round(double x) { return (long long)(x < 0 ? x - 0.5 : x + 0.5); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1231; }
    { const char * tb_v1232 = "static void tb_ps(const char *s) { fputs(s, stdout); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1232; }
    { const char * tb_v1233 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1233; }
    { const char * tb_v1234 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1234; }
    { const char * tb_v1235 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1235; }
    { const char * tb_v1236 = "// ---------- ошибки fail / try / or / else err ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1236; }
    { const char * tb_v1237 = "// Функция, которая сломалась, кладёт текст ошибки в tb_err и сразу выходит.\n"; *(const char * *)tb_grow(v_lines) = tb_v1237; }
    { const char * tb_v1238 = "// Тот, кто её вызвал, смотрит в tb_err.\n"; *(const char * *)tb_grow(v_lines) = tb_v1238; }
    { const char * tb_v1239 = "static _Thread_local const char *tb_err = 0;   // у каждого ядра своя\n"; *(const char * *)tb_grow(v_lines) = tb_v1239; }
    { const char * tb_v1240 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1240; }
    { const char * tb_v1241 = "TB_COLD static void tb_die(const char *msg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1241; }
    { const char * tb_v1242 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v1242; }
    { const char * tb_v1243 = "    fprintf(stderr, \"ошибка: %s\\n\", msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v1243; }
    { const char * tb_v1244 = "    exit(1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1244; }
    { const char * tb_v1245 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1245; }
    { const char * tb_v1246 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1246; }
    { const char * tb_v1247 = "// ---------- текст ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1247; }
    { const char * tb_v1248 = "static const char *tb_upper(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1248; }
    { const char * tb_v1249 = "    size_t n = strlen(s); char *r = tb_mem(n + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1249; }
    { const char * tb_v1250 = "    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v1250; }
    { const char * tb_v1251 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1251; }
    { const char * tb_v1252 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1252; }
    { const char * tb_v1253 = "static const char *tb_lower(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1253; }
    { const char * tb_v1254 = "    size_t n = strlen(s); char *r = tb_mem(n + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1254; }
    { const char * tb_v1255 = "    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v1255; }
    { const char * tb_v1256 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1256; }
    { const char * tb_v1257 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1257; }
    { const char * tb_v1258 = "static int tb_space(char c) { return c == ' ' || c == '\\t' || c == '\\n' || c == '\\r'; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1258; }
    { const char * tb_v1259 = "static const char *tb_trim(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1259; }
    { const char * tb_v1260 = "    size_t a = 0, b = strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1260; }
    { const char * tb_v1261 = "    while (a < b && tb_space(s[a])) a++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1261; }
    { const char * tb_v1262 = "    while (b > a && tb_space(s[b - 1])) b--;\n"; *(const char * *)tb_grow(v_lines) = tb_v1262; }
    { const char * tb_v1263 = "    char *r = tb_mem(b - a + 1); memcpy(r, s + a, b - a); r[b - a] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1263; }
    { const char * tb_v1264 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1264; }
    { const char * tb_v1265 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1265; }
    { const char * tb_v1266 = "static long long tb_find(const char *s, const char *part) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1266; }
    { const char * tb_v1267 = "    const char *p = strstr(s, part);\n"; *(const char * *)tb_grow(v_lines) = tb_v1267; }
    { const char * tb_v1268 = "    return p ? (long long)(p - s) : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1268; }
    { const char * tb_v1269 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1269; }
    { const char * tb_v1270 = "static int tb_starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1270; }
    { const char * tb_v1271 = "static int tb_ends(const char *s, const char *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1271; }
    { const char * tb_v1272 = "    size_t a = strlen(s), b = strlen(p);\n"; *(const char * *)tb_grow(v_lines) = tb_v1272; }
    { const char * tb_v1273 = "    return b <= a && strcmp(s + a - b, p) == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1273; }
    { const char * tb_v1274 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1274; }
    { const char * tb_v1275 = "static const char *tb_replace(const char *s, const char *from, const char *to, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1275; }
    { const char * tb_v1276 = "    size_t lf = strlen(from), lt = strlen(to), count = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1276; }
    { const char * tb_v1277 = "    if (lf == 0) tb_crash(line, \"replace: что заменять — пустой текст\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1277; }
    { const char * tb_v1278 = "    for (const char *p = strstr(s, from); p; p = strstr(p + lf, from)) count++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1278; }
    { const char * tb_v1279 = "    char *r = tb_mem(strlen(s) + count * lt + 1), *w = r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1279; }
    { const char * tb_v1280 = "    const char *p = s, *q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1280; }
    { const char * tb_v1281 = "    while ((q = strstr(p, from)) != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1281; }
    { const char * tb_v1282 = "        memcpy(w, p, q - p); w += q - p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1282; }
    { const char * tb_v1283 = "        memcpy(w, to, lt); w += lt;\n"; *(const char * *)tb_grow(v_lines) = tb_v1283; }
    { const char * tb_v1284 = "        p = q + lf;\n"; *(const char * *)tb_grow(v_lines) = tb_v1284; }
    { const char * tb_v1285 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1285; }
    { const char * tb_v1286 = "    strcpy(w, p);\n"; *(const char * *)tb_grow(v_lines) = tb_v1286; }
    { const char * tb_v1287 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1287; }
    { const char * tb_v1288 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1288; }
    { const char * tb_v1289 = "static tb_list *tb_split(const char *s, const char *sep, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1289; }
    { const char * tb_v1290 = "    size_t ls = strlen(sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v1290; }
    { const char * tb_v1291 = "    if (ls == 0) tb_crash(line, \"split: разделитель — пустой текст\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1291; }
    { const char * tb_v1292 = "    tb_list *l = tb_list_new(sizeof(const char *));\n"; *(const char * *)tb_grow(v_lines) = tb_v1292; }
    { const char * tb_v1293 = "    const char *p = s, *q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1293; }
    { const char * tb_v1294 = "    while (1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1294; }
    { const char * tb_v1295 = "        q = strstr(p, sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v1295; }
    { const char * tb_v1296 = "        size_t n = q ? (size_t)(q - p) : strlen(p);\n"; *(const char * *)tb_grow(v_lines) = tb_v1296; }
    { const char * tb_v1297 = "        char *part = tb_mem(n + 1); memcpy(part, p, n); part[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1297; }
    { const char * tb_v1298 = "        *(const char **)tb_grow(l) = part;\n"; *(const char * *)tb_grow(v_lines) = tb_v1298; }
    { const char * tb_v1299 = "        if (!q) break;\n"; *(const char * *)tb_grow(v_lines) = tb_v1299; }
    { const char * tb_v1300 = "        p = q + ls;\n"; *(const char * *)tb_grow(v_lines) = tb_v1300; }
    { const char * tb_v1301 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1301; }
    { const char * tb_v1302 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1302; }
    { const char * tb_v1303 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1303; }
    { const char * tb_v1304 = "static const char *tb_join(tb_list *l, const char *sep) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1304; }
    { const char * tb_v1305 = "    size_t n = 0, ls = strlen(sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v1305; }
    { const char * tb_v1306 = "    for (long long i = 0; i < l->len; i++) n += strlen(((const char **)l->data)[i]) + ls;\n"; *(const char * *)tb_grow(v_lines) = tb_v1306; }
    { const char * tb_v1307 = "    char *r = tb_mem(n + 1), *w = r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1307; }
    { const char * tb_v1308 = "    for (long long i = 0; i < l->len; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1308; }
    { const char * tb_v1309 = "        const char *part = ((const char **)l->data)[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v1309; }
    { const char * tb_v1310 = "        if (i > 0) { memcpy(w, sep, ls); w += ls; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1310; }
    { const char * tb_v1311 = "        memcpy(w, part, strlen(part)); w += strlen(part);\n"; *(const char * *)tb_grow(v_lines) = tb_v1311; }
    { const char * tb_v1312 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1312; }
    { const char * tb_v1313 = "    *w = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1313; }
    { const char * tb_v1314 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1314; }
    { const char * tb_v1315 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1315; }
    { const char * tb_v1316 = "static long long tb_to_int(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1316; }
    { const char * tb_v1317 = "    long long v = 0; int i = 0, minus = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1317; }
    { const char * tb_v1318 = "    if (s[0] == '-') { minus = 1; i = 1; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1318; }
    { const char * tb_v1319 = "    if (s[i] == 0) { tb_err = tb_cat(\"это не число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1319; }
    { const char * tb_v1320 = "    for (; s[i]; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1320; }
    { const char * tb_v1321 = "        if (s[i] < '0' || s[i] > '9') { tb_err = tb_cat(\"это не число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1321; }
    { const char * tb_v1322 = "        v = v * 10 + (s[i] - '0');\n"; *(const char * *)tb_grow(v_lines) = tb_v1322; }
    { const char * tb_v1323 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1323; }
    { const char * tb_v1324 = "    return minus ? -v : v;\n"; *(const char * *)tb_grow(v_lines) = tb_v1324; }
    { const char * tb_v1325 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1325; }
    { const char * tb_v1326 = "static double tb_to_dec(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1326; }
    { const char * tb_v1327 = "    char *end = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1327; }
    { const char * tb_v1328 = "    double v = strtod(s, &end);\n"; *(const char * *)tb_grow(v_lines) = tb_v1328; }
    { const char * tb_v1329 = "    if (s[0] == 0 || *end != 0) { tb_err = tb_cat(\"это не дробное число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1329; }
    { const char * tb_v1330 = "    return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v1330; }
    { const char * tb_v1331 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1331; }
    { const char * tb_v1332 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1332; }
    { const char * tb_v1333 = "// ---------- файлы, ввод, слова запуска ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1333; }
    { const char * tb_v1334 = "static const char *tb_read_file(const char *path) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1334; }
    { const char * tb_v1335 = "    FILE *f = fopen(path, \"rb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1335; }
    { const char * tb_v1336 = "    if (f == 0) { tb_err = tb_cat(\"не могу открыть файл: \", path); return \"\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1336; }
    { const char * tb_v1337 = "    size_t cap = 4096, n = 0, got;\n"; *(const char * *)tb_grow(v_lines) = tb_v1337; }
    { const char * tb_v1338 = "    char *buf = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1338; }
    { const char * tb_v1339 = "    while ((got = fread(buf + n, 1, cap - n - 1, f)) > 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1339; }
    { const char * tb_v1340 = "        n += got;\n"; *(const char * *)tb_grow(v_lines) = tb_v1340; }
    { const char * tb_v1341 = "        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1341; }
    { const char * tb_v1342 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1342; }
    { const char * tb_v1343 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1343; }
    { const char * tb_v1344 = "    buf[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1344; }
    { const char * tb_v1345 = "    return buf;\n"; *(const char * *)tb_grow(v_lines) = tb_v1345; }
    { const char * tb_v1346 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1346; }
    { const char * tb_v1347 = "static void tb_write_file(const char *path, const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1347; }
    { const char * tb_v1348 = "    FILE *f = fopen(path, \"wb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1348; }
    { const char * tb_v1349 = "    if (f == 0) { tb_err = tb_cat(\"не могу записать файл: \", path); return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1349; }
    { const char * tb_v1350 = "    fputs(s, f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1350; }
    { const char * tb_v1351 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1351; }
    { const char * tb_v1352 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1352; }
    { const char * tb_v1353 = "static const char *tb_input(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1353; }
    { const char * tb_v1354 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v1354; }
    { const char * tb_v1355 = "    size_t cap = 256, n = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1355; }
    { const char * tb_v1356 = "    char *buf = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1356; }
    { const char * tb_v1357 = "    int c;\n"; *(const char * *)tb_grow(v_lines) = tb_v1357; }
    { const char * tb_v1358 = "    while ((c = getchar()) != EOF && c != '\\n') {\n"; *(const char * *)tb_grow(v_lines) = tb_v1358; }
    { const char * tb_v1359 = "        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1359; }
    { const char * tb_v1360 = "        buf[n++] = (char)c;\n"; *(const char * *)tb_grow(v_lines) = tb_v1360; }
    { const char * tb_v1361 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1361; }
    { const char * tb_v1362 = "    if (c == EOF && n == 0) { tb_err = \"ввод кончился\"; return \"\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1362; }
    { const char * tb_v1363 = "    if (n > 0 && buf[n - 1] == '\\r') n--;\n"; *(const char * *)tb_grow(v_lines) = tb_v1363; }
    { const char * tb_v1364 = "    buf[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1364; }
    { const char * tb_v1365 = "    return buf;\n"; *(const char * *)tb_grow(v_lines) = tb_v1365; }
    { const char * tb_v1366 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1366; }
    { const char * tb_v1367 = "static tb_list *tb_args;\n"; *(const char * *)tb_grow(v_lines) = tb_v1367; }
    { const char * tb_v1368 = "static long long tb_run(const char *command) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1368; }
    { const char * tb_v1369 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v1369; }
    { const char * tb_v1370 = "    int code = system(command);\n"; *(const char * *)tb_grow(v_lines) = tb_v1370; }
    { const char * tb_v1371 = "    if (code == -1) return -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1371; }
    { const char * tb_v1372 = "    return code == 0 ? 0 : (code > 255 ? code / 256 : code);\n"; *(const char * *)tb_grow(v_lines) = tb_v1372; }
    { const char * tb_v1373 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1373; }
    { const char * tb_v1374 = "static void tb_exit(long long code) { fflush(stdout); exit((int)code); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1374; }
    { const char * tb_v1375 = "static int tb_file_exists(const char *path) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1375; }
    { const char * tb_v1376 = "    FILE *f = fopen(path, \"rb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1376; }
    { const char * tb_v1377 = "    if (f == 0) return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1377; }
    { const char * tb_v1378 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1378; }
    { const char * tb_v1379 = "    return 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1379; }
    { const char * tb_v1380 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1380; }
    { const char * tb_v1381 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1381; }
    { const char * tb_v1382 = "// ---------- числа ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1382; }
    { const char * tb_v1383 = "static long long tb_absi(long long x) { return x < 0 ? -x : x; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1383; }
    { const char * tb_v1384 = "static long long tb_mini(long long a, long long b) { return a < b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1384; }
    { const char * tb_v1385 = "static long long tb_maxi(long long a, long long b) { return a > b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1385; }
    { const char * tb_v1386 = "static double tb_mind(double a, double b) { return a < b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1386; }
    { const char * tb_v1387 = "static double tb_maxd(double a, double b) { return a > b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1387; }
    { const char * tb_v1388 = "static double tb_sqrt(double x, int line) { if (x < 0) tb_crash(line, \"sqrt: корень из отрицательного числа\"); return sqrt(x); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1388; }
    { const char * tb_v1389 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1389; }
    { const char * tb_v1390 = "// ---------- sort ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1390; }
    { const char * tb_v1391 = "static int tb_cmp_int(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return (x > y) - (x < y); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1391; }
    { const char * tb_v1392 = "static int tb_cmp_dec(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1392; }
    { const char * tb_v1393 = "static int tb_cmp_text(const void *a, const void *b) { return strcmp(*(const char * const *)a, *(const char * const *)b); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1393; }
    { const char * tb_v1394 = "static tb_list *tb_copy(tb_list *l) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1394; }
    { const char * tb_v1395 = "    if (l->size == 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1395; }
    { const char * tb_v1396 = "        tb_list *r = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1396; }
    { const char * tb_v1397 = "        long long bytes = l->cap / 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1397; }
    { const char * tb_v1398 = "        r->len = l->len; r->cap = l->cap; r->size = 0; r->data = tb_mem(bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v1398; }
    { const char * tb_v1399 = "        memcpy(r->data, l->data, bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v1399; }
    { const char * tb_v1400 = "        return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1400; }
    { const char * tb_v1401 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1401; }
    { const char * tb_v1402 = "    return tb_list_from(l->len, l->size, l->data);\n"; *(const char * *)tb_grow(v_lines) = tb_v1402; }
    { const char * tb_v1403 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1403; }
    { const char * tb_v1404 = "static void tb_sort_list(tb_list *l, int (*cmp)(const void *, const void *)) { qsort(l->data, l->len, l->size, cmp); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1404; }
    { const char * tb_v1405 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1405; }
    { const char * tb_v1406 = "// ---------- dict: ключи и значения лежат в двух списках по порядку добавления,\n"; *(const char * *)tb_grow(v_lines) = tb_v1406; }
    { const char * tb_v1407 = "// а найти номер по ключу можно тремя способами (mode):\n"; *(const char * *)tb_grow(v_lines) = tb_v1407; }
    { const char * tb_v1408 = "//   1 — «номер = адрес»: ключи int шли подряд base, base+1, base+2 ... — номер = ключ - base, без поиска;\n"; *(const char * *)tb_grow(v_lines) = tb_v1408; }
    { const char * tb_v1409 = "//   2 — «адресное хранение»: ключи int лежат плотно — pos[ключ - base] = номер + 1, один взгляд;\n"; *(const char * *)tb_grow(v_lines) = tb_v1409; }
    { const char * tb_v1410 = "//   0 — таблица slots с хешем (текст или разбросанные числа) ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1410; }
    { const char * tb_v1411 = "typedef struct {\n"; *(const char * *)tb_grow(v_lines) = tb_v1411; }
    { const char * tb_v1412 = "    tb_list *keys; tb_list *vals; long long *slots; long long nslots; int text;\n"; *(const char * *)tb_grow(v_lines) = tb_v1412; }
    { const char * tb_v1413 = "    int mode; long long base; long long *pos; long long npos;\n"; *(const char * *)tb_grow(v_lines) = tb_v1413; }
    { const char * tb_v1414 = "} tb_dict;\n"; *(const char * *)tb_grow(v_lines) = tb_v1414; }
    { const char * tb_v1415 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1415; }
    { const char * tb_v1416 = "static unsigned long long tb_hash(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1416; }
    { const char * tb_v1417 = "    unsigned long long h = 1469598103934665603ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v1417; }
    { const char * tb_v1418 = "    if (d->text) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1418; }
    { const char * tb_v1419 = "        for (const unsigned char *p = *(const unsigned char * const *)k; *p; p++) { h ^= *p; h *= 1099511628211ULL; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1419; }
    { const char * tb_v1420 = "    } else {\n"; *(const char * *)tb_grow(v_lines) = tb_v1420; }
    { const char * tb_v1421 = "        h ^= (unsigned long long)*(const long long *)k;\n"; *(const char * *)tb_grow(v_lines) = tb_v1421; }
    { const char * tb_v1422 = "        h *= 0x9E3779B97F4A7C15ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v1422; }
    { const char * tb_v1423 = "        h ^= h >> 32;\n"; *(const char * *)tb_grow(v_lines) = tb_v1423; }
    { const char * tb_v1424 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1424; }
    { const char * tb_v1425 = "    return h;\n"; *(const char * *)tb_grow(v_lines) = tb_v1425; }
    { const char * tb_v1426 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1426; }
    { const char * tb_v1427 = "static int tb_keq(tb_dict *d, const void *a, const void *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1427; }
    { const char * tb_v1428 = "    if (d->text) return strcmp(*(const char * const *)a, *(const char * const *)b) == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1428; }
    { const char * tb_v1429 = "    return *(const long long *)a == *(const long long *)b;\n"; *(const char * *)tb_grow(v_lines) = tb_v1429; }
    { const char * tb_v1430 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1430; }
    { const char * tb_v1431 = "static inline long long tb_dfind(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1431; }
    { const char * tb_v1432 = "    if (d->mode == 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1432; }
    { const char * tb_v1433 = "        unsigned long long at = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v1433; }
    { const char * tb_v1434 = "        return at < (unsigned long long)d->keys->len ? (long long)at : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1434; }
    { const char * tb_v1435 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1435; }
    { const char * tb_v1436 = "    if (d->mode == 2) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1436; }
    { const char * tb_v1437 = "        unsigned long long o = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v1437; }
    { const char * tb_v1438 = "        return o < (unsigned long long)d->npos ? d->pos[o] - 1 : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1438; }
    { const char * tb_v1439 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1439; }
    { const char * tb_v1440 = "    unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1440; }
    { const char * tb_v1441 = "    while (d->slots[i] != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1441; }
    { const char * tb_v1442 = "        long long at = d->slots[i] - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1442; }
    { const char * tb_v1443 = "        if (tb_keq(d, d->keys->data + at * d->keys->size, k)) return at;\n"; *(const char * *)tb_grow(v_lines) = tb_v1443; }
    { const char * tb_v1444 = "        i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1444; }
    { const char * tb_v1445 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1445; }
    { const char * tb_v1446 = "    return -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1446; }
    { const char * tb_v1447 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1447; }
    { const char * tb_v1448 = "// память для таблиц dict — там же, где лежат его ключи (временная или обычная)\n"; *(const char * *)tb_grow(v_lines) = tb_v1448; }
    { const char * tb_v1449 = "static void *tb_dalloc(tb_dict *d, void *old, long long bytes) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1449; }
    { const char * tb_v1450 = "    int in_temp = tb_nchunks > 0 && tb_in_temp(d->keys->data);\n"; *(const char * *)tb_grow(v_lines) = tb_v1450; }
    { const char * tb_v1451 = "    if (old && !in_temp) free(old);\n"; *(const char * *)tb_grow(v_lines) = tb_v1451; }
    { const char * tb_v1452 = "    void *p = in_temp ? tb_temp_alloc(bytes) : malloc(bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v1452; }
    { const char * tb_v1453 = "    if (!p) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1453; }
    { const char * tb_v1454 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1454; }
    { const char * tb_v1455 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1455; }
    { const char * tb_v1456 = "static void tb_dreindex(tb_dict *d, long long nslots) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1456; }
    { const char * tb_v1457 = "    d->mode = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1457; }
    { const char * tb_v1458 = "    if (d->pos) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1458; }
    { const char * tb_v1459 = "        if (!(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);\n"; *(const char * *)tb_grow(v_lines) = tb_v1459; }
    { const char * tb_v1460 = "        d->pos = 0; d->npos = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1460; }
    { const char * tb_v1461 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1461; }
    { const char * tb_v1462 = "    d->nslots = nslots;\n"; *(const char * *)tb_grow(v_lines) = tb_v1462; }
    { const char * tb_v1463 = "    d->slots = tb_dalloc(d, d->slots, nslots * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1463; }
    { const char * tb_v1464 = "    memset(d->slots, 0, nslots * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1464; }
    { const char * tb_v1465 = "    unsigned long long mask = nslots - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1465; }
    { const char * tb_v1466 = "    for (long long at = 0; at < d->keys->len; at++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1466; }
    { const char * tb_v1467 = "        unsigned long long i = tb_hash(d, d->keys->data + at * d->keys->size) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1467; }
    { const char * tb_v1468 = "        while (d->slots[i] != 0) i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1468; }
    { const char * tb_v1469 = "        d->slots[i] = at + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1469; }
    { const char * tb_v1470 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1470; }
    { const char * tb_v1471 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1471; }
    { const char * tb_v1472 = "// перейти на таблицу с хешем (ключ ушёл далеко от остальных или его удалили)\n"; *(const char * *)tb_grow(v_lines) = tb_v1472; }
    { const char * tb_v1473 = "static void tb_dto_hash(tb_dict *d) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1473; }
    { const char * tb_v1474 = "    long long n = 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1474; }
    { const char * tb_v1475 = "    while (n < d->keys->len * 2 + 2) n *= 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1475; }
    { const char * tb_v1476 = "    tb_dreindex(d, n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1476; }
    { const char * tb_v1477 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1477; }
    { const char * tb_v1478 = "// mode 2: таблица pos на ключи base .. base+npos-1\n"; *(const char * *)tb_grow(v_lines) = tb_v1478; }
    { const char * tb_v1479 = "static void tb_dto_pos(tb_dict *d, long long npos) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1479; }
    { const char * tb_v1480 = "    long long *p = tb_dalloc(d, 0, npos * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1480; }
    { const char * tb_v1481 = "    memset(p, 0, npos * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1481; }
    { const char * tb_v1482 = "    const long long *ks = (const long long *)d->keys->data;\n"; *(const char * *)tb_grow(v_lines) = tb_v1482; }
    { const char * tb_v1483 = "    for (long long at = 0; at < d->keys->len; at++) p[ks[at] - d->base] = at + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1483; }
    { const char * tb_v1484 = "    if (d->pos && !(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);\n"; *(const char * *)tb_grow(v_lines) = tb_v1484; }
    { const char * tb_v1485 = "    d->pos = p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1485; }
    { const char * tb_v1486 = "    d->npos = npos;\n"; *(const char * *)tb_grow(v_lines) = tb_v1486; }
    { const char * tb_v1487 = "    d->mode = 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1487; }
    { const char * tb_v1488 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1488; }
    { const char * tb_v1489 = "static tb_dict *tb_dict_new(int text, long long vsize) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1489; }
    { const char * tb_v1490 = "    tb_dict *d = tb_mem(sizeof(tb_dict));\n"; *(const char * *)tb_grow(v_lines) = tb_v1490; }
    { const char * tb_v1491 = "    d->text = text;\n"; *(const char * *)tb_grow(v_lines) = tb_v1491; }
    { const char * tb_v1492 = "    d->keys = tb_list_new(text ? sizeof(const char *) : sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v1492; }
    { const char * tb_v1493 = "    d->vals = tb_list_new(vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v1493; }
    { const char * tb_v1494 = "    d->slots = 0; d->pos = 0; d->npos = 0; d->base = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1494; }
    { const char * tb_v1495 = "    if (text) tb_dreindex(d, 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1495; }
    { const char * tb_v1496 = "    else { d->mode = 1; d->nslots = 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1496; }
    { const char * tb_v1497 = "    return d;\n"; *(const char * *)tb_grow(v_lines) = tb_v1497; }
    { const char * tb_v1498 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1498; }
    { const char * tb_v1499 = "// новый ключ int k: можно ли остаться без поиска (mode 1 или 2)\n"; *(const char * *)tb_grow(v_lines) = tb_v1499; }
    { const char * tb_v1500 = "static void tb_dplace_int(tb_dict *d, long long k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1500; }
    { const char * tb_v1501 = "    long long n = d->keys->len;          // уже вместе с новым ключом\n"; *(const char * *)tb_grow(v_lines) = tb_v1501; }
    { const char * tb_v1502 = "    if (d->mode == 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1502; }
    { const char * tb_v1503 = "        if (n == 1) { d->base = k; return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1503; }
    { const char * tb_v1504 = "        if (k == d->base + n - 1) return;  // подряд — остаёмся в mode 1\n"; *(const char * *)tb_grow(v_lines) = tb_v1504; }
    { const char * tb_v1505 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1505; }
    { const char * tb_v1506 = "    if (d->mode == 1 || d->mode == 2) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1506; }
    { const char * tb_v1507 = "        // плотно ли: ключи не дальше 4 ячеек на один ключ\n"; *(const char * *)tb_grow(v_lines) = tb_v1507; }
    { const char * tb_v1508 = "        long long o = k - d->base;\n"; *(const char * *)tb_grow(v_lines) = tb_v1508; }
    { const char * tb_v1509 = "        if (o >= 0 && o < 4 * n + 64) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1509; }
    { const char * tb_v1510 = "            if (d->mode == 2 && o < d->npos) { d->pos[o] = n; return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1510; }
    { const char * tb_v1511 = "            long long want = d->npos * 2 > o + 1 ? d->npos * 2 : o + 1 + n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1511; }
    { const char * tb_v1512 = "            if (want < 16) want = 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v1512; }
    { const char * tb_v1513 = "            tb_dto_pos(d, want);\n"; *(const char * *)tb_grow(v_lines) = tb_v1513; }
    { const char * tb_v1514 = "            return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1514; }
    { const char * tb_v1515 = "        }\n"; *(const char * *)tb_grow(v_lines) = tb_v1515; }
    { const char * tb_v1516 = "        tb_dto_hash(d);\n"; *(const char * *)tb_grow(v_lines) = tb_v1516; }
    { const char * tb_v1517 = "        return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1517; }
    { const char * tb_v1518 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1518; }
    { const char * tb_v1519 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1519; }
    { const char * tb_v1520 = "// место под значение ключа k; если ключа нет — добавить со значением *def\n"; *(const char * *)tb_grow(v_lines) = tb_v1520; }
    { const char * tb_v1521 = "static void *tb_dput(tb_dict *d, const void *k, const void *def) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1521; }
    { const char * tb_v1522 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v1522; }
    { const char * tb_v1523 = "    if (at >= 0) return d->vals->data + at * d->vals->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1523; }
    { const char * tb_v1524 = "    memcpy(tb_grow(d->keys), k, d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1524; }
    { const char * tb_v1525 = "    void *v = tb_grow(d->vals);\n"; *(const char * *)tb_grow(v_lines) = tb_v1525; }
    { const char * tb_v1526 = "    if (def) memcpy(v, def, d->vals->size); else memset(v, 0, d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1526; }
    { const char * tb_v1527 = "    if (d->mode != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1527; }
    { const char * tb_v1528 = "        // без поиска; если ключ лёг далеко, tb_dplace_int сам перейдёт на хеш и впишет все ключи\n"; *(const char * *)tb_grow(v_lines) = tb_v1528; }
    { const char * tb_v1529 = "        tb_dplace_int(d, *(const long long *)k);\n"; *(const char * *)tb_grow(v_lines) = tb_v1529; }
    { const char * tb_v1530 = "        return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v1530; }
    { const char * tb_v1531 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1531; }
    { const char * tb_v1532 = "    if (d->keys->len * 2 > d->nslots) tb_dreindex(d, d->nslots * 2);\n"; *(const char * *)tb_grow(v_lines) = tb_v1532; }
    { const char * tb_v1533 = "    else {\n"; *(const char * *)tb_grow(v_lines) = tb_v1533; }
    { const char * tb_v1534 = "        unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1534; }
    { const char * tb_v1535 = "        while (d->slots[i] != 0) i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v1535; }
    { const char * tb_v1536 = "        d->slots[i] = d->keys->len;\n"; *(const char * *)tb_grow(v_lines) = tb_v1536; }
    { const char * tb_v1537 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1537; }
    { const char * tb_v1538 = "    return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v1538; }
    { const char * tb_v1539 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1539; }
    { const char * tb_v1540 = "static void *tb_dget(tb_dict *d, const void *k, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1540; }
    { const char * tb_v1541 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v1541; }
    { const char * tb_v1542 = "    if (TB_RARE(at < 0)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1542; }
    { const char * tb_v1543 = "        if (d->text) tb_crash(line, tb_cat(\"в dict нет ключа: \", *(const char * const *)k));\n"; *(const char * *)tb_grow(v_lines) = tb_v1543; }
    { const char * tb_v1544 = "        tb_crash(line, tb_cat(\"в dict нет ключа: \", tb_itos(*(const long long *)k)));\n"; *(const char * *)tb_grow(v_lines) = tb_v1544; }
    { const char * tb_v1545 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1545; }
    { const char * tb_v1546 = "    return d->vals->data + at * d->vals->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1546; }
    { const char * tb_v1547 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1547; }
    { const char * tb_v1548 = "// together: шаг 1 — просим память принести место ключа заранее\n"; *(const char * *)tb_grow(v_lines) = tb_v1548; }
    { const char * tb_v1549 = "static inline unsigned long long tb_dpre1(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1549; }
    { const char * tb_v1550 = "    if (d->mode == 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1550; }
    { const char * tb_v1551 = "        unsigned long long at = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v1551; }
    { const char * tb_v1552 = "        if (at < (unsigned long long)d->vals->len) __builtin_prefetch(d->vals->data + at * d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1552; }
    { const char * tb_v1553 = "        return ~0ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v1553; }
    { const char * tb_v1554 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1554; }
    { const char * tb_v1555 = "    if (d->mode == 2) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1555; }
    { const char * tb_v1556 = "        unsigned long long o = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v1556; }
    { const char * tb_v1557 = "        if (o < (unsigned long long)d->npos) { __builtin_prefetch(&d->pos[o]); return o; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1557; }
    { const char * tb_v1558 = "        return ~0ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v1558; }
    { const char * tb_v1559 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1559; }
    { const char * tb_v1560 = "    unsigned long long i = tb_hash(d, k) & (d->nslots - 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1560; }
    { const char * tb_v1561 = "    __builtin_prefetch(&d->slots[i]);\n"; *(const char * *)tb_grow(v_lines) = tb_v1561; }
    { const char * tb_v1562 = "    return i;\n"; *(const char * *)tb_grow(v_lines) = tb_v1562; }
    { const char * tb_v1563 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1563; }
    { const char * tb_v1564 = "// шаг 2 — место приехало: просим принести ключ и значение\n"; *(const char * *)tb_grow(v_lines) = tb_v1564; }
    { const char * tb_v1565 = "static inline void tb_dpre2(tb_dict *d, unsigned long long i) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1565; }
    { const char * tb_v1566 = "    if (i == ~0ULL || d->mode == 1) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1566; }
    { const char * tb_v1567 = "    long long s = d->mode == 2 ? d->pos[i] : d->slots[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v1567; }
    { const char * tb_v1568 = "    if (s != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1568; }
    { const char * tb_v1569 = "        if (d->mode == 0) __builtin_prefetch(d->keys->data + (s - 1) * d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1569; }
    { const char * tb_v1570 = "        __builtin_prefetch(d->vals->data + (s - 1) * d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1570; }
    { const char * tb_v1571 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1571; }
    { const char * tb_v1572 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1572; }
    { const char * tb_v1573 = "// элемент списка заранее (prefetch за пределами списка ничего не ломает)\n"; *(const char * *)tb_grow(v_lines) = tb_v1573; }
    { const char * tb_v1574 = "static inline void tb_lpre(tb_list *l, long long i) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1574; }
    { const char * tb_v1575 = "    __builtin_prefetch(l->data + i * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1575; }
    { const char * tb_v1576 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1576; }
    { const char * tb_v1577 = "// ---------- холодные качества obj: своя память, подряд, отдельно от горячих ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1577; }
    { const char * tb_v1578 = "static void *tb_cold_mem(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1578; }
    { const char * tb_v1579 = "    if (tb_temp_depth > 0) return tb_mem(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1579; }
    { const char * tb_v1580 = "    static _Thread_local char *p = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1580; }
    { const char * tb_v1581 = "    static _Thread_local long long left = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1581; }
    { const char * tb_v1582 = "    n = (n + 15) & ~15LL;\n"; *(const char * *)tb_grow(v_lines) = tb_v1582; }
    { const char * tb_v1583 = "    if (left < n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1583; }
    { const char * tb_v1584 = "        long long size = n > 65536 ? n : 65536;\n"; *(const char * *)tb_grow(v_lines) = tb_v1584; }
    { const char * tb_v1585 = "        p = malloc(size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1585; }
    { const char * tb_v1586 = "        if (!p) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1586; }
    { const char * tb_v1587 = "        left = size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1587; }
    { const char * tb_v1588 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1588; }
    { const char * tb_v1589 = "    void *r = p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1589; }
    { const char * tb_v1590 = "    p += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1590; }
    { const char * tb_v1591 = "    left -= n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1591; }
    { const char * tb_v1592 = "    memset(r, 0, n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1592; }
    { const char * tb_v1593 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1593; }
    { const char * tb_v1594 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1594; }
    { const char * tb_v1595 = "// ---------- background: работа в своём потоке ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1595; }
    { const char * tb_v1596 = "typedef struct { pthread_t th; int done; void *res; } tb_task;\n"; *(const char * *)tb_grow(v_lines) = tb_v1596; }
    { const char * tb_v1597 = "static void tb_task_start(tb_task *t, void *(*run)(void *), void *arg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1597; }
    { const char * tb_v1598 = "    t->done = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1598; }
    { const char * tb_v1599 = "    if (pthread_create(&t->th, 0, run, arg) != 0) tb_crash(0, \"не получилось запустить background\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1599; }
    { const char * tb_v1600 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1600; }
    { const char * tb_v1601 = "// a.wait: дождаться потока (один раз) и отдать адрес ответа\n"; *(const char * *)tb_grow(v_lines) = tb_v1601; }
    { const char * tb_v1602 = "static void *tb_wait(tb_task *t) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1602; }
    { const char * tb_v1603 = "    if (!t->done) { pthread_join(t->th, 0); t->done = 1; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1603; }
    { const char * tb_v1604 = "    return t->res;\n"; *(const char * *)tb_grow(v_lines) = tb_v1604; }
    { const char * tb_v1605 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1605; }
    { const char * tb_v1606 = "static int tb_dhas(tb_dict *d, const void *k) { return tb_dfind(d, k) >= 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1606; }
    { const char * tb_v1607 = "static void tb_dremove(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1607; }
    { const char * tb_v1608 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v1608; }
    { const char * tb_v1609 = "    if (at < 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1609; }
    { const char * tb_v1610 = "    if (d->mode != 0) tb_dto_hash(d);\n"; *(const char * *)tb_grow(v_lines) = tb_v1610; }
    { const char * tb_v1611 = "    long long last = d->keys->len - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1611; }
    { const char * tb_v1612 = "    // на место удалённого ставим последний: порядок меняется только у него\n"; *(const char * *)tb_grow(v_lines) = tb_v1612; }
    { const char * tb_v1613 = "    memcpy(d->keys->data + at * d->keys->size, d->keys->data + last * d->keys->size, d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1613; }
    { const char * tb_v1614 = "    memcpy(d->vals->data + at * d->vals->size, d->vals->data + last * d->vals->size, d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1614; }
    { const char * tb_v1615 = "    d->keys->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1615; }
    { const char * tb_v1616 = "    d->vals->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1616; }
    { const char * tb_v1617 = "    tb_dreindex(d, d->nslots);\n"; *(const char * *)tb_grow(v_lines) = tb_v1617; }
    { const char * tb_v1618 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1618; }
    { const char * tb_v1619 = "static tb_dict *tb_dict_from(int text, long long vsize, long long n, const void *keys, const void *vals) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1619; }
    { const char * tb_v1620 = "    tb_dict *d = tb_dict_new(text, vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v1620; }
    { const char * tb_v1621 = "    long long ksize = d->keys->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1621; }
    { const char * tb_v1622 = "    for (long long i = 0; i < n; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1622; }
    { const char * tb_v1623 = "        memcpy(tb_dput(d, (const char *)keys + i * ksize, 0), (const char *)vals + i * vsize, vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v1623; }
    { const char * tb_v1624 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1624; }
    { const char * tb_v1625 = "    return d;\n"; *(const char * *)tb_grow(v_lines) = tb_v1625; }
    { const char * tb_v1626 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1626; }
    { const char * tb_v1627 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1627; }
    { const char * tb_v1628 = "// ---------- on all cores: цикл на всех ядрах ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1628; }
    { const char * tb_v1629 = "// Работа делится на куски; каждое ядро берёт следующий свободный кусок,\n"; *(const char * *)tb_grow(v_lines) = tb_v1629; }
    { const char * tb_v1630 = "// пока они не кончатся (так ядра не простаивают, если куски разные по тяжести).\n"; *(const char * *)tb_grow(v_lines) = tb_v1630; }
    { const char * tb_v1631 = "// shared: каждое ядро копит своё на «листке», потом один раз добавляет под замком.\n"; *(const char * *)tb_grow(v_lines) = tb_v1631; }
    { const char * tb_v1632 = "static pthread_mutex_t tb_par_lock = PTHREAD_MUTEX_INITIALIZER;\n"; *(const char * *)tb_grow(v_lines) = tb_v1632; }
    { const char * tb_v1633 = "static int tb_cores(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1633; }
    { const char * tb_v1634 = "    const char *want = getenv(\"TB_CORES\");   // TB_CORES=1 — проверить на одном ядре\n"; *(const char * *)tb_grow(v_lines) = tb_v1634; }
    { const char * tb_v1635 = "    long n = want != 0 ? atol(want) : sysconf(_SC_NPROCESSORS_ONLN);\n"; *(const char * *)tb_grow(v_lines) = tb_v1635; }
    { const char * tb_v1636 = "    return n < 1 ? 1 : (n > 256 ? 256 : (int)n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1636; }
    { const char * tb_v1637 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1637; }
    { const char * tb_v1638 = "static void tb_par_run(void *(*work)(void *), void *job, long long total) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1638; }
    { const char * tb_v1639 = "    int n = tb_cores();\n"; *(const char * *)tb_grow(v_lines) = tb_v1639; }
    { const char * tb_v1640 = "    if (n > total) n = (int)total;\n"; *(const char * *)tb_grow(v_lines) = tb_v1640; }
    { const char * tb_v1641 = "    if (n < 1) n = 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1641; }
    { const char * tb_v1642 = "    pthread_t t[256];\n"; *(const char * *)tb_grow(v_lines) = tb_v1642; }
    { const char * tb_v1643 = "    for (int i = 1; i < n; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1643; }
    { const char * tb_v1644 = "        if (pthread_create(&t[i], 0, work, job) != 0) tb_crash(0, \"не получилось запустить ядро\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1644; }
    { const char * tb_v1645 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1645; }
    { const char * tb_v1646 = "    work(job);\n"; *(const char * *)tb_grow(v_lines) = tb_v1646; }
    { const char * tb_v1647 = "    for (int i = 1; i < n; i++) pthread_join(t[i], 0);\n"; *(const char * *)tb_grow(v_lines) = tb_v1647; }
    { const char * tb_v1648 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1648; }
    { const char * tb_v1649 = "static long long tb_par_step(long long total) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1649; }
    { const char * tb_v1650 = "    long long s = total / ((long long)tb_cores() * 16);\n"; *(const char * *)tb_grow(v_lines) = tb_v1650; }
    { const char * tb_v1651 = "    return s < 1 ? 1 : s;\n"; *(const char * *)tb_grow(v_lines) = tb_v1651; }
    { const char * tb_v1652 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1652; }
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
