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
static void tb_temp_end(tb_mark m) {
    // первый кусок оставляем пустым: следующий temp возьмёт его без malloc
    while (tb_nchunks > m.n && tb_nchunks > 1) {
        tb_nchunks -= 1;
        free(tb_chunks[tb_nchunks].mem);
    }
    if (tb_nchunks > m.n) tb_chunks[0].used = 0;
    else if (tb_nchunks > 0) tb_chunks[tb_nchunks - 1].used = m.used;
    tb_temp_depth -= 1;
}
static void *tb_mem(long long n) {
    if (tb_temp_depth > 0) return tb_temp_alloc(n);
    void *p = malloc(n > 0 ? n : 1);
    if (p == 0) tb_crash(0, "не хватило памяти");
    return p;
}
// вырасти кусок: временный растёт во временной памяти, обычный — через realloc
static void *tb_resize(void *p, long long old_n, long long new_n) {
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
static const char *tb_chr(long long c) { char *s = tb_mem(2); s[0] = (char)c; s[1] = 0; return s; }
static const char *tb_char_at(const char *s, long long i, int line) {
    if (i < 0 || i >= (long long)strlen(s)) tb_crash(line, "номер за пределами текста");
    return tb_chr((unsigned char)s[i]);
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
static tb_list *tb_copy(tb_list *l) { return tb_list_from(l->len, l->size, l->data); }
static void tb_sort_list(tb_list *l, int (*cmp)(const void *, const void *)) { qsort(l->data, l->len, l->size, cmp); }

// ---------- dict: ключи и значения лежат в двух списках по порядку добавления,
// а таблица slots быстро находит номер по ключу (номер + 1, 0 — пусто) ----------
typedef struct { tb_list *keys; tb_list *vals; long long *slots; long long nslots; int text; } tb_dict;

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
static long long tb_dfind(tb_dict *d, const void *k) {
    unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;
    while (d->slots[i] != 0) {
        long long at = d->slots[i] - 1;
        if (tb_keq(d, d->keys->data + at * d->keys->size, k)) return at;
        i = (i + 1) & mask;
    }
    return -1;
}
static void tb_dreindex(tb_dict *d, long long nslots) {
    // новая таблица там же, где старая: временная — во временной памяти
    int in_temp = d->slots != 0 ? tb_nchunks > 0 && tb_in_temp(d->slots) : tb_temp_depth > 0;
    if (!in_temp) free(d->slots);
    d->nslots = nslots;
    d->slots = in_temp ? tb_temp_alloc(nslots * sizeof(long long)) : malloc(nslots * sizeof(long long));
    if (!d->slots) tb_crash(0, "не хватило памяти");
    memset(d->slots, 0, nslots * sizeof(long long));
    unsigned long long mask = nslots - 1;
    for (long long at = 0; at < d->keys->len; at++) {
        unsigned long long i = tb_hash(d, d->keys->data + at * d->keys->size) & mask;
        while (d->slots[i] != 0) i = (i + 1) & mask;
        d->slots[i] = at + 1;
    }
}
static tb_dict *tb_dict_new(int text, long long vsize) {
    tb_dict *d = tb_mem(sizeof(tb_dict));
    d->text = text;
    d->keys = tb_list_new(text ? sizeof(const char *) : sizeof(long long));
    d->vals = tb_list_new(vsize);
    d->slots = 0;
    tb_dreindex(d, 8);
    return d;
}
// место под значение ключа k; если ключа нет — добавить со значением *def
static void *tb_dput(tb_dict *d, const void *k, const void *def) {
    long long at = tb_dfind(d, k);
    if (at >= 0) return d->vals->data + at * d->vals->size;
    memcpy(tb_grow(d->keys), k, d->keys->size);
    void *v = tb_grow(d->vals);
    if (def) memcpy(v, def, d->vals->size); else memset(v, 0, d->vals->size);
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
    if (at < 0) {
        if (d->text) tb_crash(line, tb_cat("в dict нет ключа: ", *(const char * const *)k));
        tb_crash(line, tb_cat("в dict нет ключа: ", tb_itos(*(const long long *)k)));
    }
    return d->vals->data + at * d->vals->size;
}
// together: шаг 1 — место ключа в таблице, просим память принести его заранее
static inline unsigned long long tb_dpre1(tb_dict *d, const void *k) {
    unsigned long long i = tb_hash(d, k) & (d->nslots - 1);
    __builtin_prefetch(&d->slots[i]);
    return i;
}
// шаг 2 — место приехало: просим принести ключ и значение
static inline void tb_dpre2(tb_dict *d, unsigned long long i) {
    long long s = d->slots[i];
    if (s != 0) {
        __builtin_prefetch(d->keys->data + (s - 1) * d->keys->size);
        __builtin_prefetch(d->vals->data + (s - 1) * d->vals->size);
    }
}
// элемент списка заранее (prefetch за пределами списка ничего не ломает)
static inline void tb_lpre(tb_list *l, long long i) {
    __builtin_prefetch(l->data + i * l->size);
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
static void tb_show_21(tb_dict * x);
static void tb_show_22(tb_list * x);
static _Thread_local const char * tb_t1;   // для  f() or запасное
static tb_o_Token *tb_new_Token(const char * a0, const char * a1, long long a2) {
    tb_o_Token *o = tb_mem(sizeof(tb_o_Token));
    o->q_kind = a0;
    o->q_word = a1;
    o->q_line = a2;
    return o;
}
static tb_o_Node *tb_new_Node(const char * a0, long long a1, const char * a2, const char * a3, tb_o_Node * a4, _Bool a5, long long a6, _Bool a7, _Bool a8, _Bool a9, tb_o_Node * a10, _Bool a11, const char * a12, const char * a13, _Bool a14, _Bool a15, long long a16, tb_list * a17, _Bool a18, _Bool a19, _Bool a20, _Bool a21, _Bool a22, tb_o_Node * a23, tb_list * a24) {
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
    o->q_caps = a24;
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
static tb_o_Comp *tb_new_Comp(const char * a0, tb_list * a1, long long a2, tb_list * a3, tb_list * a4, tb_list * a5, tb_list * a6, tb_list * a7, const char * a8, tb_list * a9, long long a10, long long a11, long long a12, long long a13, tb_list * a14, long long a15, tb_o_Node * a16, long long a17, long long a18, _Bool a19, _Bool a20, tb_list * a21, long long a22, tb_o_Node * a23, tb_list * a24, tb_list * a25, tb_list * a26, long long a27, tb_list * a28, tb_list * a29, tb_list * a30, long long a31) {
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
static void f_emit__emit_count(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static void f_emit__emit_gives_return(tb_o_Comp * v_c, long long v_depth);
static void f_emit__emit_statement(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth);
static void f_emit__emit_block(tb_o_Comp * v_c, tb_o_Node * v_block, long long v_depth);
static void f_emit__emit_signature(tb_o_Comp * v_c, tb_o_Node * v_f);
static void f_emit__emit_func(tb_o_Comp * v_c, tb_o_Node * v_f);
static void f_emit__emit_show(tb_o_Comp * v_c, tb_o_Type * v_t);
static void f_emit__emit_cols_obj(tb_o_Comp * v_c, tb_o_Node * v_o);
static void f_emit__emit_obj(tb_o_Comp * v_c, tb_o_Node * v_o);
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
static void tb_show_21(tb_dict * x) {
    tb_ps("{");
    for (long long i = 0; i < x->keys->len; i++) {
        if (i > 0) tb_ps(", ");
        tb_show_4(((const char * *)x->keys->data)[i]);
        tb_ps(": ");
        tb_show_4(((const char * *)x->vals->data)[i]);
    }
    tb_ps("}");
}
static void tb_show_22(tb_list * x) {
    tb_ps("[");
    for (long long i = 0; i < x->len; i++) {
        if (i > 0) tb_ps(", ");
        tb_show_3(((_Bool *)x->data)[i]);
    }
    tb_ps("]");
}

static const char * f_main__folder_of(const char * v_path) {
    long long v_cut_at = 0LL;
    {
        long long tb_end1 = ((long long)strlen(v_path) - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1; v_i++) {
            if ((strcmp(tb_char_at(v_path, v_i, 14), "/") == 0)) {
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
    if ((((long long)strlen(v_path) > 3LL) && tb_ends(v_path, ".tb"))) {
        return tb_cut(v_path, 0LL, ((long long)strlen(v_path) - 4LL), 22);
    }
    return v_path;
    tb_crash(20, "функция without_tb дошла до конца без return");
    return "";
}

static const char * f_main__module_of(const char * v_path) {
    const char * v_folder = f_main__folder_of(v_path);
    return f_main__without_tb(tb_cut(v_path, (long long)strlen(v_folder), ((long long)strlen(v_path) - 1LL), 27));
    tb_crash(25, "функция module_of дошла до конца без return");
    return "";
}

static void f_main__load_module(tb_o_Comp * v_c, tb_o_Node * v_program, const char * v_path, long long v_line_of_use, const char * v_used_from, tb_list * v_loaded) {
    const char * v_name = f_main__module_of(v_path);
    {
        tb_list * tb_l2 = v_loaded;
        for (long long tb_i2 = 0; tb_i2 < tb_l2->len; tb_i2++) {
            const char * const v_m = ((const char * *)tb_l2->data)[tb_i2];
            if ((strcmp(v_m, v_name) == 0)) {
                return;
            }
        }
    }
    if (((v_loaded)->len == 200LL)) {
        f_errors__error_at(v_c, 0LL, "слишком много модулей", "");
    }
    { const char * tb_v3 = v_name; *(const char * *)tb_grow(v_loaded) = tb_v3; }
    if ((!tb_file_exists(v_path))) {
        { const char * tb_v4 = v_used_from; const char * *tb_p4 = &(((tb_o_Comp *)tb_nn(v_c, 38))->q_file_name); *tb_p4 = tb_v4; }
        f_errors__error_at(v_c, v_line_of_use, "нет файла для use", v_path);
    }
    const char * tb_v5 = tb_read_file(v_path);
    if (tb_err != 0) {
        const char * const v_err = tb_err;
        tb_err = 0;
        (tb_show_4(tb_cat("не могу открыть файл: ", v_path)), tb_ps("\n"));
        tb_exit(1LL);
    }
    const char * v_source = tb_v5;
    { const char * tb_v6 = v_path; const char * *tb_p6 = &(((tb_o_Comp *)tb_nn(v_c, 43))->q_file_name); *tb_p6 = tb_v6; }
    f_lexer__lex(v_c, v_source);
    tb_o_Node * v_part = f_parser__parse(v_c);
    {
        tb_list * tb_l7 = ((tb_o_Node *)tb_nn(v_part, 46))->q_kids;
        for (long long tb_i7 = 0; tb_i7 < tb_l7->len; tb_i7++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l7->data)[tb_i7];
            { const char * tb_v8 = v_name; const char * *tb_p8 = &(((tb_o_Node *)tb_nn(v_k, 47))->q_mod); *tb_p8 = tb_v8; }
            { const char * tb_v9 = v_path; const char * *tb_p9 = &(((tb_o_Node *)tb_nn(v_k, 48))->q_file); *tb_p9 = tb_v9; }
            f_parser__add_kid(v_program, v_k);
        }
    }
    {
        tb_list * tb_l10 = ((tb_o_Node *)tb_nn(v_part, 51))->q_kids;
        for (long long tb_i10 = 0; tb_i10 < tb_l10->len; tb_i10++) {
            tb_o_Node * const v_u = ((tb_o_Node * *)tb_l10->data)[tb_i10];
            if ((strcmp(((tb_o_Node *)tb_nn(v_u, 52))->q_kind, "use") == 0)) {
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
    { const char * tb_v11 = v_path; const char * *tb_p11 = &(((tb_o_Comp *)tb_nn(v_c, 60))->q_file_name); *tb_p11 = tb_v11; }
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
    if ((strcmp(v_name, "") == 0)) {
        v_name = ".";
    }
    while ((((long long)strlen(v_name) > 1LL) && tb_ends(v_name, "/"))) {
        v_name = tb_cut(v_name, 0LL, ((long long)strlen(v_name) - 2LL), 86);
    }
    if (f_main__is_folder(v_name)) {
        v_main_tb = tb_cat(v_name, "/main.tb");
        if ((!f_main__is_file(v_main_tb))) {
            f_main__say_error(tb_cat(tb_cat("в папке ", v_name), " нет main.tb — с него начинается проект"));
        }
        const char * v_base = tb_cut(v_name, (long long)strlen(f_main__folder_of(v_name)), ((long long)strlen(v_name) - 1LL), 91);
        const char * v_build_dir = tb_cat(v_name, "/build");
        long long v_code = tb_run(tb_cat("mkdir -p ", f_main__quote(v_build_dir)));
        if ((strcmp(v_name, ".") == 0)) {
            v_code = tb_run(tb_cat("basename \"$PWD\" > ", f_main__quote(tb_cat(v_build_dir, "/.name"))));
            v_base = tb_trim((tb_t1 = tb_read_file(tb_cat(v_build_dir, "/.name")), tb_err ? (tb_err = 0, "") : tb_t1));
        }
        if ((((strcmp(v_base, "") == 0) || (strcmp(v_base, ".") == 0)) || (strcmp(v_base, "..") == 0))) {
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
    if ((strcmp(v_name, "") == 0)) {
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
    tb_list * v_a = tb_args;
    if (((v_a)->len < 1LL)) {
        f_main__usage();
        tb_exit(1LL);
    }
    const char * v_mode = (*(const char * *)tb_at(v_a, 0LL, sizeof(const char *), 158));
    if ((strcmp(v_mode, "new") == 0)) {
        const char * v_name = "";
        if (((v_a)->len > 1LL)) {
            v_name = (*(const char * *)tb_at(v_a, 1LL, sizeof(const char *), 162));
        }
        f_main__new_project(v_name);
        return;
    }
    if ((((strcmp(v_mode, "help") == 0) || (strcmp(v_mode, "-h") == 0)) || (strcmp(v_mode, "--help") == 0))) {
        f_main__usage();
        return;
    }
    if (((strcmp(v_mode, "version") == 0) || (strcmp(v_mode, "--version") == 0))) {
        (tb_show_4("Tachbit 0.7 (компилятор на Tachbit)"), tb_ps("\n"));
        return;
    }
    if ((strcmp(v_mode, "start") == 0)) {
        v_mode = "run";
    }
    if ((((v_a)->len < 2LL) && (!((((strcmp(v_mode, "run") == 0) || (strcmp(v_mode, "build") == 0)) || (strcmp(v_mode, "check") == 0)) || (strcmp(v_mode, "c") == 0))))) {
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
        tb_r_f_main__project_main tb_m15 = f_main__project_main(v_arg);
        v_path = tb_m15.o0;
        v_exe = tb_m15.o1;
    }
    tb_o_Comp * v_c = tb_new_Comp(v_path, tb_list_new(sizeof(tb_o_Token *)), 0LL, tb_list_new(sizeof(tb_o_Type *)), tb_list_new(sizeof(tb_o_Func *)), tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Use *)), tb_list_new(sizeof(const char *)), "", tb_list_new(sizeof(tb_o_Var *)), 0LL, 0LL, 0LL, 0LL, tb_list_new(sizeof(long long)), 0LL, 0, 0LL, 0LL, 0, 0, tb_list_new(sizeof(const char *)), 0LL, 0, tb_list_new(sizeof(const char *)), tb_list_new(sizeof(const char *)), tb_list_new(sizeof(const char *)), 0LL, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(const char *)), tb_list_new(sizeof(long long)), 0LL);
    if ((strcmp(v_mode, "tokens") == 0)) {
        const char * tb_v16 = tb_read_file(v_path);
        if (tb_err != 0) {
            const char * const v_err = tb_err;
            tb_err = 0;
            f_main__say_error(tb_cat("не могу открыть файл: ", v_path));
        }
        const char * v_src = tb_v16;
        f_lexer__lex(v_c, v_src);
        f_lexer__print_tokens(v_c);
        return;
    }
    if ((strcmp(v_mode, "tree") == 0)) {
        tb_list * v_out = tb_list_new(sizeof(const char *));
        f_parser__tree_lines(f_main__load_program(v_c, v_path), 0LL, v_out);
        (tb_show_4(tb_join(v_out, "\n")), tb_ps("\n"));
        return;
    }
    if ((strcmp(v_mode, "check") == 0)) {
        f_check__check(v_c, f_main__load_program(v_c, v_path));
        (tb_show_4("ошибок нет"), tb_ps("\n"));
        return;
    }
    if ((strcmp(v_mode, "c") == 0)) {
        tb_o_Node * v_program = f_main__load_program(v_c, v_path);
        f_check__check(v_c, v_program);
        f_emit__emit_c(v_c, v_program);
        (tb_show_4(tb_cut(tb_join(((tb_o_Comp *)tb_nn(v_c, 201))->q_out, ""), 0LL, ((long long)strlen(tb_join(((tb_o_Comp *)tb_nn(v_c, 201))->q_out, "")) - 2LL), 201)), tb_ps("\n"));
        return;
    }
    if ((strcmp(v_mode, "build") == 0)) {
        tb_exit(f_main__build(v_c, v_path, v_exe));
    }
    if ((strcmp(v_mode, "run") == 0)) {
        if ((f_main__build(v_c, v_path, v_exe) != 0LL)) {
            tb_exit(1LL);
        }
        const char * v_command = f_main__quote(v_exe);
        if ((tb_find(v_exe, "/") < 0LL)) {
            v_command = f_main__quote(tb_cat("./", v_exe));
        }
        {
            long long tb_end17 = ((v_a)->len - 1LL);
            for (long long v_i = 2LL; v_i <= tb_end17; v_i++) {
                v_command = tb_cat(tb_cat(v_command, " "), f_main__quote((*(const char * *)tb_at(v_a, v_i, sizeof(const char *), 213))));
            }
        }
        tb_exit(tb_run(v_command));
    }
    f_main__usage();
    tb_exit(1LL);
}

static void f_errors__error_at(tb_o_Comp * v_c, long long v_line, const char * v_message, const char * v_detail) {
    _Bool v_color = (tb_run("[ -t 1 ] && [ -z \"$NO_COLOR\" ]") == 0LL);
    const char * v_place = ((tb_o_Comp *)tb_nn(v_c, 7))->q_file_name;
    if ((v_line > 0LL)) {
        v_place = tb_cat(tb_cat(((tb_o_Comp *)tb_nn(v_c, 9))->q_file_name, ", строка "), tb_itos(v_line));
    }
    const char * v_s = tb_cat(tb_cat(v_place, ": "), v_message);
    if ((strcmp(v_detail, "") != 0)) {
        v_s = tb_cat(tb_cat(v_s, ": "), v_detail);
    }
    if (v_color) {
        const char * v_esc = tb_chr(27LL);
        v_s = tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_esc, "[1m"), v_place), ":"), v_esc), "[0m "), v_esc), "[31m"), v_message), v_esc), "[0m");
        if ((strcmp(v_detail, "") != 0)) {
            v_s = tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_s, ": "), v_esc), "[33m"), v_detail), v_esc), "[0m");
        }
    }
    (tb_show_4(v_s), tb_ps("\n"));
    tb_exit(1LL);
}

static _Bool f_lexer__is_command(const char * v_word) {
    tb_list * v_commands = tb_list_from(40, sizeof(const char *), (const char *[]){"func", "return", "gives", "if", "else", "each", "in", "count", "from", "to", "repeat", "times", "while", "skip", "stop", "const", "shared", "obj", "link", "int", "dec", "text", "bool", "and", "or", "not", "true", "false", "use", "modpub", "modpri", "temp", "together", "background", "danger", "by", "list", "dict", "fail", "try"});
    {
        tb_list * tb_l18 = v_commands;
        for (long long tb_i18 = 0; tb_i18 < tb_l18->len; tb_i18++) {
            const char * const v_c = ((const char * *)tb_l18->data)[tb_i18];
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
    if ((v_i < (long long)strlen(v_s))) {
        return tb_code(tb_char_at(v_s, v_i, 16), 16);
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
    { tb_o_Token * tb_v19 = tb_new_Token(v_kind, v_word, v_line); *(tb_o_Token * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 26))->q_tokens) = tb_v19; }
}

static _Bool f_lexer__two_char_sign(const char * v_a, const char * v_b) {
    if (((strcmp(v_b, "=") == 0) && ((((((((strcmp(v_a, "+") == 0) || (strcmp(v_a, "-") == 0)) || (strcmp(v_a, "*") == 0)) || (strcmp(v_a, "/") == 0)) || (strcmp(v_a, "=") == 0)) || (strcmp(v_a, "!") == 0)) || (strcmp(v_a, "<") == 0)) || (strcmp(v_a, ">") == 0)))) {
        return 1;
    }
    if (((strcmp(v_a, "-") == 0) && (strcmp(v_b, ">") == 0))) {
        return 1;
    }
    if (((strcmp(v_a, ".") == 0) && (strcmp(v_b, ".") == 0))) {
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
    { tb_list * tb_v20 = v_fresh; tb_list * *tb_p20 = &(((tb_o_Comp *)tb_nn(v_c, 43))->q_tokens); *tb_p20 = tb_v20; }
    tb_list * v_indents = tb_list_from(1, sizeof(long long), (long long[]){0LL});
    tb_list * v_lines = tb_split(v_src, "\n", 45);
    long long v_line = 0LL;
    {
        tb_list * tb_l21 = v_lines;
        for (long long tb_i21 = 0; tb_i21 < tb_l21->len; tb_i21++) {
            const char * const v_s = ((const char * *)tb_l21->data)[tb_i21];
            { long long tb_v22 = 1LL; long long *tb_p22 = &(v_line); *tb_p22 += tb_v22; }
            long long v_n = (long long)strlen(v_s);
            long long v_spaces = 0LL;
            long long v_pos = 0LL;
            while (((v_pos < v_n) && ((f_lexer__at(v_s, v_pos) == 32LL) || (f_lexer__at(v_s, v_pos) == 9LL)))) {
                if ((f_lexer__at(v_s, v_pos) == 9LL)) {
                    { long long tb_v23 = 4LL; long long *tb_p23 = &(v_spaces); *tb_p23 += tb_v23; }
                } else {
                    { long long tb_v24 = 1LL; long long *tb_p24 = &(v_spaces); *tb_p24 += tb_v24; }
                }
                { long long tb_v25 = 1LL; long long *tb_p25 = &(v_pos); *tb_p25 += tb_v25; }
            }
            if ((((v_pos >= v_n) || (f_lexer__at(v_s, v_pos) == 13LL)) || ((f_lexer__at(v_s, v_pos) == 47LL) && (f_lexer__at(v_s, (v_pos + 1LL)) == 47LL)))) {
                continue;
            }
            long long v_top = (*(long long *)tb_at(v_indents, ((v_indents)->len - 1LL), sizeof(long long), 62));
            if ((v_spaces > v_top)) {
                if (((v_indents)->len == 200LL)) {
                    f_errors__error_at(v_c, v_line, "слишком глубокая вложенность", "");
                }
                { long long tb_v26 = v_spaces; *(long long *)tb_grow(v_indents) = tb_v26; }
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
                    { long long tb_v27 = 1LL; long long *tb_p27 = &(v_pos); *tb_p27 += tb_v27; }
                    continue;
                }
                if (((v_ch == 47LL) && (f_lexer__at(v_s, (v_pos + 1LL)) == 47LL))) {
                    break;
                }
                long long v_start = v_pos;
                if (f_lexer__is_letter(v_ch)) {
                    while ((f_lexer__is_letter(f_lexer__at(v_s, v_pos)) || f_lexer__is_digit(f_lexer__at(v_s, v_pos)))) {
                        { long long tb_v28 = 1LL; long long *tb_p28 = &(v_pos); *tb_p28 += tb_v28; }
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
                        { long long tb_v29 = 1LL; long long *tb_p29 = &(v_pos); *tb_p29 += tb_v29; }
                    }
                    if (((f_lexer__at(v_s, v_pos) == 46LL) && f_lexer__is_digit(f_lexer__at(v_s, (v_pos + 1LL))))) {
                        v_kind = "decimal";
                        { long long tb_v30 = 1LL; long long *tb_p30 = &(v_pos); *tb_p30 += tb_v30; }
                        while (f_lexer__is_digit(f_lexer__at(v_s, v_pos))) {
                            { long long tb_v31 = 1LL; long long *tb_p31 = &(v_pos); *tb_p31 += tb_v31; }
                        }
                    }
                    f_lexer__add_token(v_c, v_kind, tb_cut(v_s, v_start, (v_pos - 1LL), 101), v_line);
                } else if ((v_ch == 34LL)) {
                    { long long tb_v32 = 1LL; long long *tb_p32 = &(v_pos); *tb_p32 += tb_v32; }
                    while (((v_pos < v_n) && (f_lexer__at(v_s, v_pos) != 34LL))) {
                        if ((f_lexer__at(v_s, v_pos) == 92LL)) {
                            { long long tb_v33 = 1LL; long long *tb_p33 = &(v_pos); *tb_p33 += tb_v33; }
                        }
                        { long long tb_v34 = 1LL; long long *tb_p34 = &(v_pos); *tb_p34 += tb_v34; }
                    }
                    if ((f_lexer__at(v_s, v_pos) != 34LL)) {
                        f_errors__error_at(v_c, v_line, "текст в кавычках не закрыт", "");
                    }
                    { long long tb_v35 = 1LL; long long *tb_p35 = &(v_pos); *tb_p35 += tb_v35; }
                    f_lexer__add_token(v_c, "string", tb_cut(v_s, v_start, (v_pos - 1LL), 111), v_line);
                } else {
                    const char * v_a = tb_char_at(v_s, v_pos, 113);
                    const char * v_b = "";
                    if (((v_pos + 1LL) < v_n)) {
                        v_b = tb_char_at(v_s, (v_pos + 1LL), 116);
                    }
                    if (f_lexer__two_char_sign(v_a, v_b)) {
                        f_lexer__add_token(v_c, "sign", tb_cat(v_a, v_b), v_line);
                        { long long tb_v36 = 2LL; long long *tb_p36 = &(v_pos); *tb_p36 += tb_v36; }
                    } else if (f_lexer__one_char_sign(v_a)) {
                        f_lexer__add_token(v_c, "sign", v_a, v_line);
                        { long long tb_v37 = 1LL; long long *tb_p37 = &(v_pos); *tb_p37 += tb_v37; }
                    } else {
                        f_errors__error_at(v_c, v_line, "непонятный символ", v_a);
                    }
                }
            }
            f_lexer__add_token(v_c, "newline", "", v_line);
        }
    }
    if ((((long long)strlen(v_src) > 0LL) && tb_ends(v_src, "\n"))) {
        { long long tb_v38 = 1LL; long long *tb_p38 = &(v_line); *tb_p38 -= tb_v38; }
    }
    while (((v_indents)->len > 1LL)) {
        long long v_gone = (*(long long *)tb_pop(v_indents, 130));
        f_lexer__add_token(v_c, "dedent", "", (v_line + 1LL));
    }
    f_lexer__add_token(v_c, "end", "", (v_line + 1LL));
}

static const char * f_lexer__token_kind_name(const char * v_kind) {
    if ((strcmp(v_kind, "name") == 0)) {
        return "ИМЯ";
    }
    if ((strcmp(v_kind, "command") == 0)) {
        return "КОМАНДА";
    }
    if ((strcmp(v_kind, "number") == 0)) {
        return "ЧИСЛО";
    }
    if ((strcmp(v_kind, "decimal") == 0)) {
        return "ДРОБЬ";
    }
    if ((strcmp(v_kind, "string") == 0)) {
        return "ТЕКСТ";
    }
    if ((strcmp(v_kind, "sign") == 0)) {
        return "ЗНАК";
    }
    if ((strcmp(v_kind, "newline") == 0)) {
        return "КОНЕЦ СТРОКИ";
    }
    if ((strcmp(v_kind, "indent") == 0)) {
        return "ОТСТУП";
    }
    if ((strcmp(v_kind, "dedent") == 0)) {
        return "ВЫСТУП";
    }
    return "КОНЕЦ ФАЙЛА";
    tb_crash(134, "функция token_kind_name дошла до конца без return");
    return "";
}

static void f_lexer__print_tokens(tb_o_Comp * v_c) {
    tb_list * v_out = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l39 = ((tb_o_Comp *)tb_nn(v_c, 158))->q_tokens;
        for (long long tb_i39 = 0; tb_i39 < tb_l39->len; tb_i39++) {
            tb_o_Token * const v_t = ((tb_o_Token * *)tb_l39->data)[tb_i39];
            const char * v_num = tb_cat("", tb_itos(((tb_o_Token *)tb_nn(v_t, 159))->q_line));
            while (((long long)strlen(v_num) < 3LL)) {
                v_num = tb_cat(" ", v_num);
            }
            const char * v_name = f_lexer__token_kind_name(((tb_o_Token *)tb_nn(v_t, 162))->q_kind);
            while (((long long)strlen(v_name) < 13LL)) {
                v_name = tb_cat(v_name, " ");
            }
            { const char * tb_v40 = tb_cat(tb_cat(tb_cat(tb_cat(v_num, ": "), v_name), " "), ((tb_o_Token *)tb_nn(v_t, 165))->q_word); *(const char * *)tb_grow(v_out) = tb_v40; }
        }
    }
    (tb_show_4(tb_join(v_out, "\n")), tb_ps("\n"));
}

static tb_o_Node * f_parser__new_node(const char * v_kind, long long v_line) {
    return tb_new_Node(v_kind, v_line, "", "", 0, 0, (-1LL), 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    tb_crash(7, "функция new_node дошла до конца без return");
    return 0;
}

static void f_parser__add_kid(tb_o_Node * v_parent, tb_o_Node * v_kid) {
    { tb_o_Node * tb_v41 = v_kid; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_parent, 11))->q_kids) = tb_v41; }
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
    if ((strcmp(((tb_o_Token *)tb_nn((*(tb_o_Token * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 27))->q_tokens, ((tb_o_Comp *)tb_nn(v_c, 27))->q_pos, sizeof(tb_o_Token *), 27)), 27))->q_kind, "end") != 0)) {
        { long long tb_v42 = 1LL; long long *tb_p42 = &(((tb_o_Comp *)tb_nn(v_c, 28))->q_pos); *tb_p42 += tb_v42; }
    }
}

static _Bool f_parser__is_sign(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__now(v_c);
    return ((strcmp(((tb_o_Token *)tb_nn(v_t, 32))->q_kind, "sign") == 0) && (strcmp(((tb_o_Token *)tb_nn(v_t, 32))->q_word, v_s) == 0));
    tb_crash(30, "функция is_sign дошла до конца без return");
    return 0;
}

static _Bool f_parser__is_cmd(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__now(v_c);
    return ((strcmp(((tb_o_Token *)tb_nn(v_t, 36))->q_kind, "command") == 0) && (strcmp(((tb_o_Token *)tb_nn(v_t, 36))->q_word, v_s) == 0));
    tb_crash(34, "функция is_cmd дошла до конца без return");
    return 0;
}

static _Bool f_parser__ahead_is_sign(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__ahead(v_c);
    return ((strcmp(((tb_o_Token *)tb_nn(v_t, 40))->q_kind, "sign") == 0) && (strcmp(((tb_o_Token *)tb_nn(v_t, 40))->q_word, v_s) == 0));
    tb_crash(38, "функция ahead_is_sign дошла до конца без return");
    return 0;
}

static const char * f_parser__what_is_here(tb_o_Comp * v_c) {
    const char * v_k = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 43))->q_kind;
    if ((strcmp(v_k, "newline") == 0)) {
        return "конец строки";
    }
    if ((strcmp(v_k, "indent") == 0)) {
        return "лишний отступ";
    }
    if ((strcmp(v_k, "dedent") == 0)) {
        return "конец блока";
    }
    if ((strcmp(v_k, "end") == 0)) {
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
    if ((strcmp(((tb_o_Token *)tb_nn(v_t, 66))->q_kind, "name") != 0)) {
        if ((strcmp(((tb_o_Token *)tb_nn(v_t, 67))->q_kind, "command") == 0)) {
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
    if ((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 74))->q_kind, "newline") != 0)) {
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
        { const char * tb_v43 = "dict"; const char * *tb_p43 = &(((tb_o_Node *)tb_nn(v_t, 86))->q_name); *tb_p43 = tb_v43; }
        f_parser__next(v_c);
        tb_o_Node * v_key = f_parser__parse_type(v_c);
        if (((strcmp(((tb_o_Node *)tb_nn(v_key, 89))->q_name, "text") != 0) && (strcmp(((tb_o_Node *)tb_nn(v_key, 89))->q_name, "int") != 0))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_key, 90))->q_line, "ключ у dict бывает text или int", ((tb_o_Node *)tb_nn(v_key, 90))->q_name);
        }
        f_parser__add_kid(v_t, v_key);
        if ((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 92))->q_kind, "newline") == 0)) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после ключа напиши тип значения: dict text int", "");
        }
        f_parser__add_kid(v_t, f_parser__parse_type(v_c));
        return v_t;
    }
    if (f_parser__at_type_word(v_c)) {
        { const char * tb_v44 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 97))->q_word; const char * *tb_p44 = &(((tb_o_Node *)tb_nn(v_t, 97))->q_name); *tb_p44 = tb_v44; }
        f_parser__next(v_c);
        if ((strcmp(((tb_o_Node *)tb_nn(v_t, 99))->q_name, "list") == 0)) {
            if (((!f_parser__at_type_word(v_c)) && (strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 100))->q_kind, "name") != 0))) {
                f_errors__error_at(v_c, f_parser__line_now(v_c), "после list напиши, что в нём лежит: list int, list text ...", "");
            }
            f_parser__add_kid(v_t, f_parser__parse_type(v_c));
        }
        return v_t;
    }
    if ((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 104))->q_kind, "name") == 0)) {
        { const char * tb_v45 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 105))->q_word; const char * *tb_p45 = &(((tb_o_Node *)tb_nn(v_t, 105))->q_name); *tb_p45 = tb_v45; }
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
    { const char * tb_v46 = v_sign; const char * *tb_p46 = &(((tb_o_Node *)tb_nn(v_n, 115))->q_value); *tb_p46 = tb_v46; }
    f_parser__add_kid(v_n, v_left);
    f_parser__add_kid(v_n, v_right);
    return v_n;
    return v_result;
}

static tb_o_Node * f_parser__parse_call(tb_o_Comp * v_c, const char * v_name, long long v_line) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_call = f_parser__new_node("call", v_line);
    { const char * tb_v47 = v_name; const char * *tb_p47 = &(((tb_o_Node *)tb_nn(v_call, 122))->q_name); *tb_p47 = tb_v47; }
    f_parser__expect_sign(v_c, "(");
    while ((!f_parser__is_sign(v_c, ")"))) {
        tb_o_Node * v_arg = f_parser__new_node("arg", f_parser__line_now(v_c));
        if (((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 127))->q_kind, "name") == 0) && f_parser__ahead_is_sign(v_c, ":"))) {
            { const char * tb_v48 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 128))->q_word; const char * *tb_p48 = &(((tb_o_Node *)tb_nn(v_arg, 128))->q_name); *tb_p48 = tb_v48; }
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
    if ((((strcmp(((tb_o_Token *)tb_nn(v_t, 143))->q_kind, "number") == 0) || (strcmp(((tb_o_Token *)tb_nn(v_t, 143))->q_kind, "decimal") == 0)) || (strcmp(((tb_o_Token *)tb_nn(v_t, 143))->q_kind, "string") == 0))) {
        v_n = f_parser__new_node(((tb_o_Token *)tb_nn(v_t, 144))->q_kind, v_line);
        { const char * tb_v49 = ((tb_o_Token *)tb_nn(v_t, 145))->q_word; const char * *tb_p49 = &(((tb_o_Node *)tb_nn(v_n, 145))->q_value); *tb_p49 = tb_v49; }
        f_parser__next(v_c);
    } else if ((f_parser__is_cmd(v_c, "true") || f_parser__is_cmd(v_c, "false"))) {
        v_n = f_parser__new_node("bool", v_line);
        { const char * tb_v50 = ((tb_o_Token *)tb_nn(v_t, 149))->q_word; const char * *tb_p50 = &(((tb_o_Node *)tb_nn(v_n, 149))->q_value); *tb_p50 = tb_v50; }
        f_parser__next(v_c);
    } else if ((strcmp(((tb_o_Token *)tb_nn(v_t, 151))->q_kind, "name") == 0)) {
        f_parser__next(v_c);
        if (f_parser__is_sign(v_c, "(")) {
            v_n = f_parser__parse_call(v_c, ((tb_o_Token *)tb_nn(v_t, 154))->q_word, v_line);
        } else {
            v_n = f_parser__new_node("name", v_line);
            { const char * tb_v51 = ((tb_o_Token *)tb_nn(v_t, 157))->q_word; const char * *tb_p51 = &(((tb_o_Node *)tb_nn(v_n, 157))->q_name); *tb_p51 = tb_v51; }
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
                { const char * tb_v52 = v_name; const char * *tb_p52 = &(((tb_o_Node *)tb_nn(v_m, 201))->q_name); *tb_p52 = tb_v52; }
                f_parser__add_kid(v_m, v_n);
                {
                    tb_list * tb_l53 = ((tb_o_Node *)tb_nn(v_call, 203))->q_kids;
                    for (long long tb_i53 = 0; tb_i53 < tb_l53->len; tb_i53++) {
                        tb_o_Node * const v_k = ((tb_o_Node * *)tb_l53->data)[tb_i53];
                        f_parser__add_kid(v_m, v_k);
                    }
                }
                v_n = v_m;
            } else {
                tb_o_Node * v_f = f_parser__new_node("field", v_dot_line);
                { const char * tb_v54 = v_name; const char * *tb_p54 = &(((tb_o_Node *)tb_nn(v_f, 208))->q_name); *tb_p54 = tb_v54; }
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
        { const char * tb_v55 = "-"; const char * *tb_p55 = &(((tb_o_Node *)tb_nn(v_n, 216))->q_value); *tb_p55 = tb_v55; }
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
        { const char * tb_v56 = "not"; const char * *tb_p56 = &(((tb_o_Node *)tb_nn(v_n, 254))->q_value); *tb_p56 = tb_v56; }
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
    if (((strcmp(((tb_o_Token *)tb_nn(v_t, 300))->q_kind, "name") == 0) && (strcmp(((tb_o_Token *)tb_nn(v_t, 300))->q_word, "on") == 0))) {
        f_parser__next(v_c);
        tb_o_Token * v_a = f_parser__now(v_c);
        tb_o_Token * v_b = f_parser__ahead(v_c);
        if (((strcmp(((tb_o_Token *)tb_nn(v_a, 304))->q_word, "all") != 0) || (strcmp(((tb_o_Token *)tb_nn(v_b, 304))->q_word, "cores") != 0))) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после on пишется all cores: each x in nums on all cores", "");
        }
        f_parser__next(v_c);
        f_parser__next(v_c);
        { _Bool tb_v57 = 1; _Bool *tb_p57 = &(((tb_o_Node *)tb_nn(v_n, 308))->q_is_par); *tb_p57 = tb_v57; }
    }
}

static tb_o_Node * f_parser__parse_each(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_n = f_parser__new_node("each", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "each");
    { const char * tb_v58 = f_parser__expect_name(v_c); const char * *tb_p58 = &(((tb_o_Node *)tb_nn(v_n, 313))->q_name); *tb_p58 = tb_v58; }
    f_parser__expect_cmd(v_c, "in");
    f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
    f_parser__parse_on_all_cores(v_c, v_n);
    if (f_parser__is_cmd(v_c, "by")) {
        f_parser__next(v_c);
        if (((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 321))->q_kind, "name") == 0) && (strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 321))->q_word, "vec") == 0))) {
            f_parser__next(v_c);
            { _Bool tb_v59 = 1; _Bool *tb_p59 = &(((tb_o_Node *)tb_nn(v_n, 323))->q_by_vec); *tb_p59 = tb_v59; }
            if (((tb_o_Node *)tb_nn(v_n, 324))->q_is_par) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 325))->q_line, "on all cores и by vec вместе пока нельзя", "");
            }
        } else {
            { tb_o_Node * tb_v60 = f_parser__parse_expr(v_c); tb_o_Node * *tb_p60 = &(((tb_o_Node *)tb_nn(v_n, 328))->q_piece_size); *tb_p60 = tb_v60; }
            { _Bool tb_v61 = 1; _Bool *tb_p61 = &(((tb_o_Node *)tb_nn(v_n, 329))->q_by_piece); *tb_p61 = tb_v61; }
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
    { const char * tb_v62 = f_parser__expect_name(v_c); const char * *tb_p62 = &(((tb_o_Node *)tb_nn(v_n, 339))->q_name); *tb_p62 = tb_v62; }
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
        if (((strcmp(((tb_o_Node *)tb_nn(v_call, 371))->q_kind, "call") != 0) && (strcmp(((tb_o_Node *)tb_nn(v_call, 371))->q_kind, "method") != 0))) {
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
        { const char * tb_v63 = f_parser__expect_name(v_c); const char * *tb_p63 = &(((tb_o_Node *)tb_nn(v_e, 384))->q_name); *tb_p63 = tb_v63; }
        f_parser__expect_newline(v_c);
        f_parser__add_kid(v_e, v_v);
        f_parser__add_kid(v_e, f_parser__parse_block(v_c));
        return v_e;
    }
    return v_v;
    return v_result;
}

static void f_parser__end_statement(tb_o_Comp * v_c, tb_o_Node * v_value) {
    if ((strcmp(((tb_o_Node *)tb_nn(v_value, 393))->q_kind, "else_err") != 0)) {
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
    { const char * tb_v64 = f_parser__expect_name(v_c); const char * *tb_p64 = &(((tb_o_Node *)tb_nn(v_n, 403))->q_name); *tb_p64 = tb_v64; }
    { _Bool tb_v65 = v_is_const; _Bool *tb_p65 = &(((tb_o_Node *)tb_nn(v_n, 404))->q_is_const); *tb_p65 = tb_v65; }
    f_parser__expect_sign(v_c, "=");
    if ((f_parser__at_type_word(v_c) || f_parser__is_cmd(v_c, "dict"))) {
        { tb_o_Node * tb_v66 = f_parser__parse_type(v_c); tb_o_Node * *tb_p66 = &(((tb_o_Node *)tb_nn(v_n, 407))->q_tsyn); *tb_p66 = tb_v66; }
        { _Bool tb_v67 = 1; _Bool *tb_p67 = &(((tb_o_Node *)tb_nn(v_n, 408))->q_has_tsyn); *tb_p67 = tb_v67; }
    }
    if ((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 409))->q_kind, "newline") != 0)) {
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
    { const char * tb_v68 = f_parser__expect_name(v_c); const char * *tb_p68 = &(((tb_o_Node *)tb_nn(v_n, 424))->q_name); *tb_p68 = tb_v68; }
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
        { const char * tb_v69 = f_parser__expect_name(v_c); const char * *tb_p69 = &(((tb_o_Node *)tb_nn(v_name, 438))->q_name); *tb_p69 = tb_v69; }
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
        if ((((strcmp(v_k, "name") != 0) && (strcmp(v_k, "index") != 0)) && (strcmp(v_k, "field") != 0))) {
            f_errors__error_at(v_c, v_line, "слева от знака должно быть имя, nums[i] или u.age", "");
        }
        tb_o_Node * v_n = f_parser__new_node("set", v_line);
        { const char * tb_v70 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 464))->q_word; const char * *tb_p70 = &(((tb_o_Node *)tb_nn(v_n, 464))->q_value); *tb_p70 = tb_v70; }
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
        { const char * tb_v71 = f_parser__expect_name(v_c); const char * *tb_p71 = &(((tb_o_Node *)tb_nn(v_e, 475))->q_name); *tb_p71 = tb_v71; }
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
        tb_list * tb_l72 = tb_list_from(4, sizeof(const char *), (const char *[]){"gives", "obj", "danger", "by"});
        for (long long tb_i72 = 0; tb_i72 < tb_l72->len; tb_i72++) {
            const char * const v_w = ((const char * *)tb_l72->data)[tb_i72];
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
        if ((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 509))->q_kind, "newline") != 0)) {
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
        if (((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 547))->q_kind, "name") != 0) || (!f_parser__ahead_is_sign(v_c, "=")))) {
            f_errors__error_at(v_c, v_line, "после shared пишется новая переменная: shared y = int 0", "");
        }
        tb_o_Node * v_n = f_parser__parse_assign(v_c, 0);
        { _Bool tb_v73 = 1; _Bool *tb_p73 = &(((tb_o_Node *)tb_nn(v_n, 550))->q_is_shared); *tb_p73 = tb_v73; }
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
    if ((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 562))->q_kind, "name") == 0)) {
        return f_parser__parse_name_statement(v_c);
    }
    if (((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 564))->q_kind, "command") == 0) && f_parser__ahead_is_sign(v_c, "="))) {
        f_errors__error_at(v_c, v_line, "это слово занято языком, возьми другое имя", ((tb_o_Token *)tb_nn(f_parser__now(v_c), 565))->q_word);
    }
    f_errors__error_at(v_c, v_line, "не понимаю, что делает строка, начиная с", f_parser__what_is_here(v_c));
    return f_parser__new_node("missing", v_line);
    return v_result;
}

static tb_o_Node * f_parser__parse_block(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_block = f_parser__new_node("block", f_parser__line_now(v_c));
    if ((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 571))->q_kind, "indent") != 0)) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "после этой строки нужен блок с отступом", "");
    }
    f_parser__next(v_c);
    while (((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 574))->q_kind, "dedent") != 0) && (strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 574))->q_kind, "end") != 0))) {
        if ((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 575))->q_kind, "indent") == 0)) {
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
        { _Bool tb_v74 = 1; _Bool *tb_p74 = &(((tb_o_Node *)tb_nn(v_p, 588))->q_is_link); *tb_p74 = tb_v74; }
        { const char * tb_v75 = f_parser__expect_name(v_c); const char * *tb_p75 = &(((tb_o_Node *)tb_nn(v_p, 589))->q_name); *tb_p75 = tb_v75; }
        return v_p;
    }
    { const char * tb_v76 = f_parser__expect_name(v_c); const char * *tb_p76 = &(((tb_o_Node *)tb_nn(v_p, 591))->q_name); *tb_p76 = tb_v76; }
    f_parser__expect_sign(v_c, ":");
    { tb_o_Node * tb_v77 = f_parser__parse_type(v_c); tb_o_Node * *tb_p77 = &(((tb_o_Node *)tb_nn(v_p, 593))->q_tsyn); *tb_p77 = tb_v77; }
    { _Bool tb_v78 = 1; _Bool *tb_p78 = &(((tb_o_Node *)tb_nn(v_p, 594))->q_has_tsyn); *tb_p78 = tb_v78; }
    return v_p;
    return v_result;
}

static tb_o_Node * f_parser__parse_func(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_f = f_parser__new_node("func", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "func");
    { const char * tb_v79 = f_parser__expect_name(v_c); const char * *tb_p79 = &(((tb_o_Node *)tb_nn(v_f, 601))->q_name); *tb_p79 = tb_v79; }
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
    { const char * tb_v80 = f_parser__expect_name(v_c); const char * *tb_p80 = &(((tb_o_Node *)tb_nn(v_o, 630))->q_name); *tb_p80 = tb_v80; }
    if (f_parser__is_cmd(v_c, "by")) {
        f_parser__next(v_c);
        if (((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 634))->q_kind, "name") != 0) || (strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 634))->q_word, "columns") != 0))) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после obj ... by пишется columns: obj Point by columns", "");
        }
        f_parser__next(v_c);
        { _Bool tb_v81 = 1; _Bool *tb_p81 = &(((tb_o_Node *)tb_nn(v_o, 637))->q_by_cols); *tb_p81 = tb_v81; }
    }
    f_parser__expect_newline(v_c);
    if ((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 639))->q_kind, "indent") != 0)) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "у obj ниже с отступом пишутся качества: name: text", "");
    }
    f_parser__next(v_c);
    while (((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 642))->q_kind, "dedent") != 0) && (strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 642))->q_kind, "end") != 0))) {
        f_parser__add_kid(v_o, f_parser__parse_param(v_c, 0));
        f_parser__expect_newline(v_c);
    }
    f_parser__next(v_c);
    return v_o;
    return v_result;
}

static tb_o_Node * f_parser__parse(tb_o_Comp * v_c) {
    { long long tb_v82 = 0LL; long long *tb_p82 = &(((tb_o_Comp *)tb_nn(v_c, 649))->q_pos); *tb_p82 = tb_v82; }
    tb_o_Node * v_program = f_parser__new_node("program", 1LL);
    while ((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 651))->q_kind, "end") != 0)) {
        if (f_parser__is_cmd(v_c, "use")) {
            tb_o_Node * v_u = f_parser__new_node("use", f_parser__line_now(v_c));
            f_parser__next(v_c);
            { const char * tb_v83 = f_parser__expect_name(v_c); const char * *tb_p83 = &(((tb_o_Node *)tb_nn(v_u, 656))->q_name); *tb_p83 = tb_v83; }
            f_parser__expect_newline(v_c);
            f_parser__add_kid(v_program, v_u);
        } else if ((f_parser__is_cmd(v_c, "modpub") || f_parser__is_cmd(v_c, "modpri"))) {
            _Bool v_is_pub = f_parser__is_cmd(v_c, "modpub");
            f_parser__next(v_c);
            if ((!f_parser__is_cmd(v_c, "func"))) {
                f_errors__error_at(v_c, f_parser__line_now(v_c), "modpub и modpri ставятся перед func", "");
            }
            tb_o_Node * v_f = f_parser__parse_func(v_c);
            { _Bool tb_v84 = v_is_pub; _Bool *tb_p84 = &(((tb_o_Node *)tb_nn(v_f, 665))->q_is_pub); *tb_p84 = tb_v84; }
            f_parser__add_kid(v_program, v_f);
        } else if (f_parser__is_cmd(v_c, "func")) {
            f_parser__add_kid(v_program, f_parser__parse_func(v_c));
        } else if (f_parser__is_cmd(v_c, "obj")) {
            f_parser__add_kid(v_program, f_parser__parse_obj(v_c));
        } else if (f_parser__is_cmd(v_c, "link")) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "подключение C-файлов через link будет в версии 0.9 (вместе с danger); файлы Tachbit подключает use", "");
        } else if ((strcmp(((tb_o_Token *)tb_nn(f_parser__now(v_c), 673))->q_kind, "indent") == 0)) {
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
    const char * v_s = "";
    {
        long long tb_end85 = v_depth;
        for (long long v_i = 1LL; v_i <= tb_end85; v_i++) {
            v_s = tb_cat(v_s, "  ");
        }
    }
    v_s = tb_cat(v_s, f_parser__node_kind_name(((tb_o_Node *)tb_nn(v_n, 689))->q_kind));
    if ((strcmp(((tb_o_Node *)tb_nn(v_n, 690))->q_name, "") != 0)) {
        v_s = tb_cat(tb_cat(v_s, " "), ((tb_o_Node *)tb_nn(v_n, 691))->q_name);
    }
    if ((strcmp(((tb_o_Node *)tb_nn(v_n, 692))->q_value, "") != 0)) {
        v_s = tb_cat(tb_cat(v_s, " "), ((tb_o_Node *)tb_nn(v_n, 693))->q_value);
    }
    if (((tb_o_Node *)tb_nn(v_n, 694))->q_is_const) {
        v_s = tb_cat(v_s, " (const)");
    }
    if (((tb_o_Node *)tb_nn(v_n, 696))->q_is_link) {
        v_s = tb_cat(v_s, " (link)");
    }
    if (((tb_o_Node *)tb_nn(v_n, 698))->q_is_shared) {
        v_s = tb_cat(v_s, " (shared)");
    }
    if (((tb_o_Node *)tb_nn(v_n, 700))->q_is_par) {
        v_s = tb_cat(v_s, " (on all cores)");
    }
    if (((tb_o_Node *)tb_nn(v_n, 702))->q_by_cols) {
        v_s = tb_cat(v_s, " (by columns)");
    }
    if (((tb_o_Node *)tb_nn(v_n, 704))->q_by_vec) {
        v_s = tb_cat(v_s, " (by vec)");
    }
    { const char * tb_v86 = v_s; *(const char * *)tb_grow(v_out) = tb_v86; }
    if (((tb_o_Node *)tb_nn(v_n, 707))->q_has_tsyn) {
        f_parser__tree_lines(((tb_o_Node *)tb_nn(v_n, 708))->q_tsyn, (v_depth + 1LL), v_out);
    }
    {
        tb_list * tb_l87 = ((tb_o_Node *)tb_nn(v_n, 709))->q_kids;
        for (long long tb_i87 = 0; tb_i87 < tb_l87->len; tb_i87++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l87->data)[tb_i87];
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
        tb_list * tb_l88 = ((tb_o_Comp *)tb_nn(v_c, 18))->q_types;
        for (long long tb_i88 = 0; tb_i88 < tb_l88->len; tb_i88++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l88->data)[tb_i88];
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
        { tb_o_Node * tb_v89 = v_onode; tb_o_Node * *tb_p89 = &(((tb_o_Type *)tb_nn(v_t, 25))->q_obj_node); *tb_p89 = tb_v89; }
    }
    if (v_has_func) {
        { tb_o_Node * tb_v90 = v_fnode; tb_o_Node * *tb_p90 = &(((tb_o_Type *)tb_nn(v_t, 27))->q_func_node); *tb_p90 = tb_v90; }
    }
    { tb_o_Type * tb_v91 = v_t; *(tb_o_Type * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 28))->q_types) = tb_v91; }
    return ((tb_o_Type *)tb_nn(v_t, 29))->q_id;
    return v_r;
}

static long long f_check__simple_type(tb_o_Comp * v_c, const char * v_kind) {
    tb_o_Node * v_nothing = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    return f_check__make_type_key(v_c, v_kind, (-1LL), (-1LL), v_nothing, 0, v_nothing, 0);
    tb_crash(31, "функция simple_type дошла до конца без return");
    return 0LL;
}

static long long f_check__list_of(tb_o_Comp * v_c, long long v_elem) {
    tb_o_Node * v_nothing = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    return f_check__make_type_key(v_c, "list", (-1LL), v_elem, v_nothing, 0, v_nothing, 0);
    tb_crash(35, "функция list_of дошла до конца без return");
    return 0LL;
}

static long long f_check__task_of(tb_o_Comp * v_c, long long v_elem) {
    tb_o_Node * v_nothing = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    return f_check__make_type_key(v_c, "task", (-1LL), v_elem, v_nothing, 0, v_nothing, 0);
    tb_crash(40, "функция task_of дошла до конца без return");
    return 0LL;
}

static long long f_check__dict_of(tb_o_Comp * v_c, long long v_key, long long v_value) {
    tb_o_Node * v_nothing = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    return f_check__make_type_key(v_c, "dict", v_key, v_value, v_nothing, 0, v_nothing, 0);
    tb_crash(44, "функция dict_of дошла до конца без return");
    return 0LL;
}

static long long f_check__obj_type(tb_o_Comp * v_c, tb_o_Node * v_o) {
    tb_o_Node * v_nothing = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    return f_check__make_type_key(v_c, "obj", (-1LL), (-1LL), v_o, 1, v_nothing, 0);
    tb_crash(48, "функция obj_type дошла до конца без return");
    return 0LL;
}

static long long f_check__multi_type(tb_o_Comp * v_c, tb_o_Node * v_fnode) {
    tb_o_Node * v_nothing = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Var *)));
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
    if ((strcmp(((tb_o_Type *)tb_nn(v_ty, 65))->q_kind, "list") == 0)) {
        return tb_cat("list ", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 66))->q_elem));
    }
    if ((strcmp(((tb_o_Type *)tb_nn(v_ty, 67))->q_kind, "dict") == 0)) {
        return tb_cat(tb_cat(tb_cat("dict ", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 68))->q_key)), " "), f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 68))->q_elem));
    }
    if ((strcmp(((tb_o_Type *)tb_nn(v_ty, 69))->q_kind, "obj") == 0)) {
        return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn(v_ty, 70))->q_obj_node, 70))->q_name;
    }
    if ((strcmp(((tb_o_Type *)tb_nn(v_ty, 71))->q_kind, "multi") == 0)) {
        return "несколько выходов";
    }
    if ((strcmp(((tb_o_Type *)tb_nn(v_ty, 73))->q_kind, "none") == 0)) {
        return "ничего";
    }
    if ((strcmp(((tb_o_Type *)tb_nn(v_ty, 75))->q_kind, "task") == 0)) {
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
    return (strcmp(f_check__kind_of(v_c, v_t), "list") == 0);
    tb_crash(82, "функция is_list дошла до конца без return");
    return 0;
}

static _Bool f_check__is_dict(tb_o_Comp * v_c, long long v_t) {
    return (strcmp(f_check__kind_of(v_c, v_t), "dict") == 0);
    tb_crash(85, "функция is_dict дошла до конца без return");
    return 0;
}

static _Bool f_check__is_cols_obj(tb_o_Comp * v_c, long long v_t) {
    return ((strcmp(f_check__kind_of(v_c, v_t), "obj") == 0) && ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 90))->q_types, v_t, sizeof(tb_o_Type *), 90)), 90))->q_obj_node, 90))->q_by_cols);
    tb_crash(89, "функция is_cols_obj дошла до конца без return");
    return 0;
}

static _Bool f_check__is_cols_list(tb_o_Comp * v_c, long long v_t) {
    return ((strcmp(f_check__kind_of(v_c, v_t), "list") == 0) && f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 94))->q_types, v_t, sizeof(tb_o_Type *), 94)), 94))->q_elem));
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
        tb_list * tb_l92 = tb_list_from(29, sizeof(const char *), (const char *[]){"print", "len", "round", "code", "char", "cut", "to_int", "to_dec", "read_file", "write_file", "input", "args", "split", "join", "find", "replace", "upper", "lower", "trim", "starts_with", "ends_with", "sort", "abs", "min", "max", "sqrt", "run", "exit", "file_exists"});
        for (long long tb_i92 = 0; tb_i92 < tb_l92->len; tb_i92++) {
            const char * const v_b = ((const char * *)tb_l92->data)[tb_i92];
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
    return (((((strcmp(v_name, "to_int") == 0) || (strcmp(v_name, "to_dec") == 0)) || (strcmp(v_name, "read_file") == 0)) || (strcmp(v_name, "write_file") == 0)) || (strcmp(v_name, "input") == 0));
    tb_crash(116, "функция builtin_can_fail дошла до конца без return");
    return 0;
}

static _Bool f_check__is_module(tb_o_Comp * v_c, const char * v_name) {
    {
        tb_list * tb_l93 = ((tb_o_Comp *)tb_nn(v_c, 123))->q_all_mods;
        for (long long tb_i93 = 0; tb_i93 < tb_l93->len; tb_i93++) {
            const char * const v_m = ((const char * *)tb_l93->data)[tb_i93];
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
        tb_list * tb_l94 = ((tb_o_Comp *)tb_nn(v_c, 131))->q_uses;
        for (long long tb_i94 = 0; tb_i94 < tb_l94->len; tb_i94++) {
            tb_o_Use * const v_u = ((tb_o_Use * *)tb_l94->data)[tb_i94];
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
        long long tb_end95 = ((((tb_o_Comp *)tb_nn(v_c, 138))->q_funcs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end95; v_i++) {
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
        long long tb_end96 = ((((tb_o_Comp *)tb_nn(v_c, 150))->q_funcs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end96; v_i++) {
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
        tb_list * tb_l97 = ((tb_o_Comp *)tb_nn(v_c, 160))->q_funcs;
        for (long long tb_i97 = 0; tb_i97 < tb_l97->len; tb_i97++) {
            tb_o_Func * const v_f = ((tb_o_Func * *)tb_l97->data)[tb_i97];
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
        long long tb_end98 = ((((tb_o_Comp *)tb_nn(v_c, 169))->q_objs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end98; v_i++) {
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
        long long tb_end99 = ((((tb_o_Node *)tb_nn(v_o, 175))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end99; v_i++) {
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
    if ((strcmp(((tb_o_Node *)tb_nn(v_t, 181))->q_name, "int") == 0)) {
        return 1LL;
    }
    if ((strcmp(((tb_o_Node *)tb_nn(v_t, 183))->q_name, "dec") == 0)) {
        return 2LL;
    }
    if ((strcmp(((tb_o_Node *)tb_nn(v_t, 185))->q_name, "text") == 0)) {
        return 4LL;
    }
    if ((strcmp(((tb_o_Node *)tb_nn(v_t, 187))->q_name, "bool") == 0)) {
        return 3LL;
    }
    if ((strcmp(((tb_o_Node *)tb_nn(v_t, 189))->q_name, "list") == 0)) {
        return f_check__list_of(v_c, f_check__resolve_type(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 190))->q_kids, 0LL, sizeof(tb_o_Node *), 190))));
    }
    if ((strcmp(((tb_o_Node *)tb_nn(v_t, 191))->q_name, "dict") == 0)) {
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
        { long long tb_v100 = 1LL; long long *tb_p100 = &(v_i); *tb_p100 -= tb_v100; }
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
    { tb_o_Var * tb_v101 = tb_new_Var(v_name, v_type, v_is_const, v_is_link, ((tb_o_Comp *)tb_nn(v_c, 224))->q_depth, 0, ""); *(tb_o_Var * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 224))->q_vars) = tb_v101; }
}

static void f_check__cut_vars(tb_o_Comp * v_c, long long v_count_left) {
    while (((((tb_o_Comp *)tb_nn(v_c, 227))->q_vars)->len > v_count_left)) {
        tb_o_Var * v_gone = (*(tb_o_Var * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 228))->q_vars, 228));
    }
}

static void f_check__open_scope(tb_o_Comp * v_c) {
    { long long tb_v102 = 1LL; long long *tb_p102 = &(((tb_o_Comp *)tb_nn(v_c, 231))->q_depth); *tb_p102 += tb_v102; }
}

static void f_check__close_scope(tb_o_Comp * v_c) {
    while ((((((tb_o_Comp *)tb_nn(v_c, 234))->q_vars)->len > 0LL) && (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 234))->q_vars, ((((tb_o_Comp *)tb_nn(v_c, 234))->q_vars)->len - 1LL), sizeof(tb_o_Var *), 234)), 234))->q_depth == ((tb_o_Comp *)tb_nn(v_c, 234))->q_depth))) {
        tb_o_Var * v_gone = (*(tb_o_Var * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 235))->q_vars, 235));
    }
    { long long tb_v103 = 1LL; long long *tb_p103 = &(((tb_o_Comp *)tb_nn(v_c, 236))->q_depth); *tb_p103 -= tb_v103; }
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
    if ((((strcmp(((tb_o_Node *)tb_nn(v_n, 251))->q_kind, "index") == 0) || (strcmp(((tb_o_Node *)tb_nn(v_n, 251))->q_kind, "field") == 0)) || (strcmp(((tb_o_Node *)tb_nn(v_n, 251))->q_kind, "method") == 0))) {
        return f_check__root_var(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 252))->q_kids, 0LL, sizeof(tb_o_Node *), 252)));
    }
    if ((strcmp(((tb_o_Node *)tb_nn(v_n, 253))->q_kind, "name") == 0)) {
        return f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 254))->q_name);
    }
    return (-1LL);
    return v_r;
}

static void f_check__capture(tb_o_Comp * v_c, long long v_v) {
    tb_o_Var * v_var = (*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 259))->q_vars, v_v, sizeof(tb_o_Var *), 259));
    {
        tb_list * tb_l104 = ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 260))->q_par_node, 260))->q_caps;
        for (long long tb_i104 = 0; tb_i104 < tb_l104->len; tb_i104++) {
            tb_o_Var * const v_old = ((tb_o_Var * *)tb_l104->data)[tb_i104];
            if (((strcmp(((tb_o_Var *)tb_nn(v_old, 261))->q_name, ((tb_o_Var *)tb_nn(v_var, 261))->q_name) == 0) && ((!((tb_o_Comp *)tb_nn(v_c, 261))->q_par_vec) || (!((tb_o_Var *)tb_nn(v_old, 261))->q_is_shared)))) {
                return;
            }
        }
    }
    { tb_o_Var * tb_v105 = tb_new_Var(((tb_o_Var *)tb_nn(v_var, 263))->q_name, ((tb_o_Var *)tb_nn(v_var, 263))->q_type, 0, ((tb_o_Var *)tb_nn(v_var, 263))->q_is_link, 0LL, (((tb_o_Var *)tb_nn(v_var, 263))->q_is_shared && (!((tb_o_Comp *)tb_nn(v_c, 263))->q_par_vec)), ""); *(tb_o_Var * *)tb_grow(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 263))->q_par_node, 263))->q_caps) = tb_v105; }
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
    if ((strcmp(f_check__kind_of(v_c, v_t), "multi") == 0)) {
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
    if (((strcmp(v_op, "or") == 0) && f_check__can_fail_call(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 305))->q_kids, 0LL, sizeof(tb_o_Node *), 305))))) {
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
        { long long tb_v106 = v_a; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 315))->q_temp_types) = tb_v106; }
        { long long tb_v107 = ((((tb_o_Comp *)tb_nn(v_c, 316))->q_temp_types)->len - 1LL); long long *tb_p107 = &(((tb_o_Node *)tb_nn(v_n, 316))->q_temp_no); *tb_p107 = tb_v107; }
        { const char * tb_v108 = "orelse"; const char * *tb_p108 = &(((tb_o_Node *)tb_nn(v_n, 317))->q_value); *tb_p108 = tb_v108; }
        return v_a;
    }
    long long v_a = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 319))->q_kids, 0LL, sizeof(tb_o_Node *), 319)));
    long long v_b = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 320))->q_kids, 1LL, sizeof(tb_o_Node *), 320)));
    if (((strcmp(v_op, "and") == 0) || (strcmp(v_op, "or") == 0))) {
        if (((v_a != 3LL) || (v_b != 3LL))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 323))->q_line, "and / or работают только с true и false", "");
        }
        return 3LL;
    }
    if (((strcmp(v_op, "==") == 0) || (strcmp(v_op, "!=") == 0))) {
        if (((!(f_check__is_number(v_a) && f_check__is_number(v_b))) && (v_a != v_b))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 327))->q_line, "нельзя сравнить разные типы", "");
        }
        if ((!f_check__is_scalar(v_a))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 329))->q_line, "списки и объекты пока нельзя сравнивать", "");
        }
        return 3LL;
    }
    if (((((strcmp(v_op, "<") == 0) || (strcmp(v_op, ">") == 0)) || (strcmp(v_op, "<=") == 0)) || (strcmp(v_op, ">=") == 0))) {
        if (((v_a == 4LL) && (v_b == 4LL))) {
            return 3LL;
        }
        if (((!f_check__is_number(v_a)) || (!f_check__is_number(v_b)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 335))->q_line, "больше / меньше работают с числами или с двумя текстами", v_op);
        }
        return 3LL;
    }
    if (((strcmp(v_op, "+") == 0) && ((v_a == 4LL) || (v_b == 4LL)))) {
        if (((!f_check__is_scalar(v_a)) || (!f_check__is_scalar(v_b)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 339))->q_line, "список или объект нельзя сложить с текстом", "");
        }
        return 4LL;
    }
    if (((!f_check__is_number(v_a)) || (!f_check__is_number(v_b)))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 342))->q_line, "эта операция работает только с числами", v_op);
    }
    if (((strcmp(v_op, "%") == 0) && ((v_a == 2LL) || (v_b == 2LL)))) {
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
        long long tb_end109 = ((((tb_o_Node *)tb_nn(v_n, 353))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end109; v_i++) {
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
    if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 365))->q_kids, v_i, sizeof(tb_o_Node *), 365)), 365))->q_name, "") != 0)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 366))->q_line, "у встроенных функций аргументы пишутся без имён", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 366))->q_kids, v_i, sizeof(tb_o_Node *), 366)), 366))->q_name);
    }
    long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 367))->q_kids, v_i, sizeof(tb_o_Node *), 367)), 367))->q_kids, 0LL, sizeof(tb_o_Node *), 367)));
    { long long tb_v110 = v_t; long long *tb_p110 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 368))->q_kids, v_i, sizeof(tb_o_Node *), 368)), 368))->q_ty); *tb_p110 = tb_v110; }
    return v_t;
    return v_r;
}

static void f_check__builtin_args(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_pattern, const char * v_usage) {
    if (((((tb_o_Node *)tb_nn(v_n, 374))->q_kids)->len != (long long)strlen(v_pattern))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 375))->q_line, "не то количество значений, правильно так", v_usage);
    }
    {
        long long tb_end111 = ((long long)strlen(v_pattern) - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end111; v_i++) {
            long long v_t = f_check__builtin_arg(v_c, v_n, v_i);
            const char * v_p = tb_char_at(v_pattern, v_i, 378);
            _Bool v_ok = 1;
            if ((strcmp(v_p, "t") == 0)) {
                v_ok = (v_t == 4LL);
            } else if ((strcmp(v_p, "i") == 0)) {
                v_ok = (v_t == 1LL);
            } else if ((strcmp(v_p, "n") == 0)) {
                v_ok = f_check__is_number(v_t);
            } else if ((strcmp(v_p, "L") == 0)) {
                v_ok = (f_check__is_list(v_c, v_t) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 387))->q_types, v_t, sizeof(tb_o_Type *), 387)), 387))->q_elem == 4LL));
            } else if ((strcmp(v_p, "s") == 0)) {
                v_ok = (f_check__is_list(v_c, v_t) && (((((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 1LL) || (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 2LL)) || (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 4LL)));
            } else if ((strcmp(v_p, "x") == 0)) {
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
    if ((strcmp(v_name, "print") == 0)) {
        {
            long long tb_end112 = ((((tb_o_Node *)tb_nn(v_n, 398))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end112; v_i++) {
                long long v_t = f_check__builtin_arg(v_c, v_n, v_i);
            }
        }
        return 0LL;
    }
    if ((strcmp(v_name, "len") == 0)) {
        f_check__builtin_args(v_c, v_n, "x", "len(список, текст или dict)");
        return 1LL;
    }
    if ((strcmp(v_name, "round") == 0)) {
        f_check__builtin_args(v_c, v_n, "n", "round(число)");
        return 1LL;
    }
    if ((strcmp(v_name, "code") == 0)) {
        f_check__builtin_args(v_c, v_n, "t", "code(\"A\")  →  65");
        return 1LL;
    }
    if ((strcmp(v_name, "char") == 0)) {
        f_check__builtin_args(v_c, v_n, "i", "char(66)  →  \"B\"");
        return 4LL;
    }
    if ((strcmp(v_name, "cut") == 0)) {
        f_check__builtin_args(v_c, v_n, "tii", "cut(текст, с какого номера, по какой номер)");
        return 4LL;
    }
    if ((strcmp(v_name, "to_int") == 0)) {
        f_check__builtin_args(v_c, v_n, "t", "to_int(\"42\")");
        return 1LL;
    }
    if ((strcmp(v_name, "to_dec") == 0)) {
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
    if ((strcmp(v_name, "input") == 0)) {
        f_check__builtin_args(v_c, v_n, "", "input()");
        return 4LL;
    }
    if ((strcmp(v_name, "args") == 0)) {
        f_check__builtin_args(v_c, v_n, "", "args()");
        return f_check__list_of(v_c, 4LL);
    }
    if ((strcmp(v_name, "split") == 0)) {
        f_check__builtin_args(v_c, v_n, "tt", "split(текст, разделитель)");
        return f_check__list_of(v_c, 4LL);
    }
    if ((strcmp(v_name, "join") == 0)) {
        f_check__builtin_args(v_c, v_n, "Lt", "join(список text, разделитель)");
        return 4LL;
    }
    if ((strcmp(v_name, "find") == 0)) {
        f_check__builtin_args(v_c, v_n, "tt", "find(текст, что ищем)  →  номер или -1");
        return 1LL;
    }
    if ((strcmp(v_name, "replace") == 0)) {
        f_check__builtin_args(v_c, v_n, "ttt", "replace(текст, что, на что)");
        return 4LL;
    }
    if ((((strcmp(v_name, "upper") == 0) || (strcmp(v_name, "lower") == 0)) || (strcmp(v_name, "trim") == 0))) {
        f_check__builtin_args(v_c, v_n, "t", "upper(текст)");
        return 4LL;
    }
    if (((strcmp(v_name, "starts_with") == 0) || (strcmp(v_name, "ends_with") == 0))) {
        f_check__builtin_args(v_c, v_n, "tt", "starts_with(текст, начало)");
        return 3LL;
    }
    if ((strcmp(v_name, "sort") == 0)) {
        f_check__builtin_args(v_c, v_n, "s", "sort(список int, dec или text)");
        return 0LL;
    }
    if ((strcmp(v_name, "run") == 0)) {
        f_check__builtin_args(v_c, v_n, "t", "run(\"команда терминала\")  →  код выхода int");
        return 1LL;
    }
    if ((strcmp(v_name, "exit") == 0)) {
        f_check__builtin_args(v_c, v_n, "i", "exit(код)");
        return 0LL;
    }
    if ((strcmp(v_name, "file_exists") == 0)) {
        f_check__builtin_args(v_c, v_n, "t", "file_exists(\"путь\")");
        return 3LL;
    }
    if ((strcmp(v_name, "abs") == 0)) {
        f_check__builtin_args(v_c, v_n, "n", "abs(число)");
        return ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 466))->q_kids, 0LL, sizeof(tb_o_Node *), 466)), 466))->q_ty;
    }
    if ((strcmp(v_name, "sqrt") == 0)) {
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
    tb_list * v_taken = tb_list_new(sizeof(_Bool));
    {
        long long tb_end113 = v_np;
        for (long long v_j = 1LL; v_j <= tb_end113; v_j++) {
            { tb_o_Node * tb_v114 = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Var *))); *(tb_o_Node * *)tb_grow(v_ordered) = tb_v114; }
            { _Bool tb_v115 = 0; *(_Bool *)tb_grow(v_taken) = tb_v115; }
        }
    }
    _Bool v_named = 0;
    {
        long long tb_end116 = ((((tb_o_Node *)tb_nn(v_call, 488))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end116; v_i++) {
            tb_o_Node * v_arg = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_call, 489))->q_kids, v_i, sizeof(tb_o_Node *), 489));
            long long v_slot = v_i;
            if ((strcmp(((tb_o_Node *)tb_nn(v_arg, 491))->q_name, "") != 0)) {
                v_named = 1;
                v_slot = (-1LL);
                {
                    long long tb_end117 = (v_np - 1LL);
                    for (long long v_j = 0LL; v_j <= tb_end117; v_j++) {
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
            if ((*(_Bool *)tb_at(v_taken, v_slot, sizeof(_Bool), 501))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_arg, 502))->q_line, "параметр указан два раза", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 502))->q_kids, v_slot, sizeof(tb_o_Node *), 502)), 502))->q_name);
            }
            { tb_o_Node * tb_v118 = v_arg; tb_o_Node * *tb_p118 = &((*(tb_o_Node * *)tb_at(v_ordered, v_slot, sizeof(tb_o_Node *), 503))); *tb_p118 = tb_v118; }
            { _Bool tb_v119 = 1; _Bool *tb_p119 = &((*(_Bool *)tb_at(v_taken, v_slot, sizeof(_Bool), 504))); *tb_p119 = tb_v119; }
        }
    }
    { tb_list * tb_v120 = v_ordered; tb_list * *tb_p120 = &(((tb_o_Node *)tb_nn(v_call, 506))->q_kids); *tb_p120 = tb_v120; }
}

static void f_check__need_place_for(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_why, _Bool v_for_link) {
    if (((strcmp(((tb_o_Node *)tb_nn(v_n, 510))->q_kind, "index") == 0) && f_check__is_dict(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 510))->q_kids, 0LL, sizeof(tb_o_Node *), 510)), 510))->q_ty))) {
        if (v_for_link) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 512))->q_line, "ярлык на значение в dict нельзя: dict может переложить значения", "");
        }
        return;
    }
    if ((strcmp(((tb_o_Node *)tb_nn(v_n, 514))->q_kind, "name") == 0)) {
        long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 515))->q_name);
        if (((v_v >= 0LL) && ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 516))->q_vars, v_v, sizeof(tb_o_Var *), 516)), 516))->q_is_const)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 517))->q_line, "это const или переменная цикла, её нельзя менять", ((tb_o_Node *)tb_nn(v_n, 517))->q_name);
        }
        return;
    }
    if ((strcmp(((tb_o_Node *)tb_nn(v_n, 519))->q_kind, "field") == 0)) {
        return;
    }
    if ((((strcmp(((tb_o_Node *)tb_nn(v_n, 521))->q_kind, "index") == 0) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 521))->q_kids, 0LL, sizeof(tb_o_Node *), 521)), 521))->q_ty)) && v_for_link)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 522))->q_line, "ярлык на элемент списка колонок нельзя: возьми ярлык на качество, например pts[i].x", "");
    }
    if (((strcmp(((tb_o_Node *)tb_nn(v_n, 523))->q_kind, "index") == 0) && f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 523))->q_kids, 0LL, sizeof(tb_o_Node *), 523)), 523))->q_ty))) {
        return;
    }
    if ((strcmp(((tb_o_Node *)tb_nn(v_n, 525))->q_kind, "index") == 0)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 526))->q_line, "буквы в тексте менять нельзя, собери новый текст", "");
    }
    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 527))->q_line, v_why, "");
}

static void f_check__need_place(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_why) {
    f_check__need_place_for(v_c, v_n, v_why, 0);
}

static long long f_check__check_new_obj(tb_o_Comp * v_c, tb_o_Node * v_n, tb_o_Node * v_o) {
    long long v_r = 0LL;
    f_check__order_args(v_c, v_n, v_o, 1);
    {
        long long tb_end121 = ((((tb_o_Node *)tb_nn(v_n, 534))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end121; v_i++) {
            if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 535))->q_kids, v_i, sizeof(tb_o_Node *), 535)), 535))->q_kind, "missing") == 0)) {
                continue;
            }
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 537))->q_kids, v_i, sizeof(tb_o_Node *), 537)), 537))->q_kids, 0LL, sizeof(tb_o_Node *), 537));
            long long v_t = f_check__check_value(v_c, v_value);
            if ((!f_check__fits(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 539))->q_kids, v_i, sizeof(tb_o_Node *), 539)), 539))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 540))->q_line, "качество другого типа", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 540))->q_kids, v_i, sizeof(tb_o_Node *), 540)), 540))->q_name);
            }
            { long long tb_v122 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 541))->q_kids, v_i, sizeof(tb_o_Node *), 541)), 541))->q_ty; long long *tb_p122 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 541))->q_kids, v_i, sizeof(tb_o_Node *), 541)), 541))->q_ty); *tb_p122 = tb_v122; }
        }
    }
    { tb_o_Node * tb_v123 = v_o; tb_o_Node * *tb_p123 = &(((tb_o_Node *)tb_nn(v_n, 542))->q_ref); *tb_p123 = tb_v123; }
    { _Bool tb_v124 = 1; _Bool *tb_p124 = &(((tb_o_Node *)tb_nn(v_n, 543))->q_has_ref); *tb_p124 = tb_v124; }
    return f_check__obj_type(v_c, v_o);
    return v_r;
}

static long long f_check__check_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    if (f_check__is_builtin(((tb_o_Node *)tb_nn(v_n, 547))->q_name)) {
        if ((f_check__builtin_can_fail(((tb_o_Node *)tb_nn(v_n, 548))->q_name) && (!((tb_o_Node *)tb_nn(v_n, 548))->q_creates))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 549))->q_line, "это может закончиться ошибкой: напиши  try ...,  ... or запасное  или  ... else err", ((tb_o_Node *)tb_nn(v_n, 549))->q_name);
        }
        return f_check__check_builtin(v_c, v_n);
    }
    if ((f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 551))->q_name) >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 552))->q_line, "это переменная, а не функция", ((tb_o_Node *)tb_nn(v_n, 552))->q_name);
    }
    long long v_o = f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_n, 553))->q_name);
    if ((v_o >= 0LL)) {
        return f_check__check_new_obj(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 555))->q_objs, v_o, sizeof(tb_o_Node *), 555)));
    }
    long long v_f = f_check__find_func(v_c, ((tb_o_Node *)tb_nn(v_n, 556))->q_name);
    if ((v_f < 0LL)) {
        f_check__explain_missing_func(v_c, v_n);
    }
    return f_check__check_user_call(v_c, v_n, v_f);
    return v_r;
}

static long long f_check__check_user_call(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_fi) {
    long long v_r = 0LL;
    tb_o_Func * v_f = (*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 563))->q_funcs, v_fi, sizeof(tb_o_Func *), 563));
    if ((strcmp(((tb_o_Node *)tb_nn(v_n, 564))->q_name, "main") == 0)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 565))->q_line, "main запускается сама, её нельзя вызывать", "");
    }
    if ((((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 566))->q_node, 566))->q_can_fail && (!((tb_o_Node *)tb_nn(v_n, 566))->q_creates))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 567))->q_line, "функция может закончиться ошибкой: напиши  try f(),  f() or запасное  или  f() else err", ((tb_o_Node *)tb_nn(v_n, 567))->q_name);
    }
    { tb_o_Node * tb_v125 = ((tb_o_Func *)tb_nn(v_f, 568))->q_node; tb_o_Node * *tb_p125 = &(((tb_o_Node *)tb_nn(v_n, 568))->q_ref); *tb_p125 = tb_v125; }
    { _Bool tb_v126 = 1; _Bool *tb_p126 = &(((tb_o_Node *)tb_nn(v_n, 569))->q_has_ref); *tb_p126 = tb_v126; }
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 570))->q_node, 570))->q_kids, 0LL, sizeof(tb_o_Node *), 570));
    f_check__order_args(v_c, v_n, v_params, 0);
    {
        long long tb_end127 = ((((tb_o_Node *)tb_nn(v_n, 572))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end127; v_i++) {
            tb_o_Node * v_p = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 573))->q_kids, v_i, sizeof(tb_o_Node *), 573));
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 574))->q_kids, v_i, sizeof(tb_o_Node *), 574)), 574))->q_kids, 0LL, sizeof(tb_o_Node *), 574));
            long long v_t = f_check__check_value(v_c, v_value);
            if (((tb_o_Node *)tb_nn(v_p, 576))->q_is_link) {
                f_check__need_place_for(v_c, v_value, "в ярлык link можно передать только переменную, u.age или nums[i]", 1);
                f_check__no_outside_change(v_c, v_value);
                if ((f_check__outside_temp(v_c, f_check__root_var(v_c, v_value)) && (!f_check__is_plain(v_t)))) {
                    f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_value, 580))->q_line, ((tb_o_Node *)tb_nn(v_p, 580))->q_name);
                }
                if ((((tb_o_Node *)tb_nn(v_p, 581))->q_ty < 0LL)) {
                    { long long tb_v128 = v_t; long long *tb_p128 = &(((tb_o_Node *)tb_nn(v_p, 583))->q_ty); *tb_p128 = tb_v128; }
                } else if ((((tb_o_Node *)tb_nn(v_p, 584))->q_ty != v_t)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 585))->q_line, "этот ярлык уже был другого типа", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_p, 585))->q_ty));
                }
            } else if ((!f_check__fits(((tb_o_Node *)tb_nn(v_p, 586))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 587))->q_line, "параметр ждёт другой тип", ((tb_o_Node *)tb_nn(v_p, 587))->q_name);
            }
            { long long tb_v129 = ((tb_o_Node *)tb_nn(v_p, 588))->q_ty; long long *tb_p129 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 588))->q_kids, v_i, sizeof(tb_o_Node *), 588)), 588))->q_ty); *tb_p129 = tb_v129; }
        }
    }
    if ((((tb_o_Func *)tb_nn(v_f, 589))->q_state == 0LL)) {
        long long v_saved_current = ((tb_o_Comp *)tb_nn(v_c, 591))->q_current;
        long long v_saved_loops = ((tb_o_Comp *)tb_nn(v_c, 592))->q_loop_depth;
        long long v_saved_vars = (((tb_o_Comp *)tb_nn(v_c, 593))->q_vars)->len;
        long long v_saved_depth = ((tb_o_Comp *)tb_nn(v_c, 594))->q_depth;
        long long v_saved_base = ((tb_o_Comp *)tb_nn(v_c, 595))->q_var_base;
        const char * v_saved_mod = ((tb_o_Comp *)tb_nn(v_c, 596))->q_cur_mod;
        const char * v_saved_file = ((tb_o_Comp *)tb_nn(v_c, 597))->q_file_name;
        long long v_saved_par = ((tb_o_Comp *)tb_nn(v_c, 598))->q_par_base;
        long long v_saved_temp = ((tb_o_Comp *)tb_nn(v_c, 599))->q_temp_base;
        long long v_saved_temp_loops = ((tb_o_Comp *)tb_nn(v_c, 600))->q_temp_loops;
        long long v_saved_pieces = ((tb_o_Comp *)tb_nn(v_c, 601))->q_piece_depth;
        { long long tb_v130 = (-1LL); long long *tb_p130 = &(((tb_o_Comp *)tb_nn(v_c, 602))->q_par_base); *tb_p130 = tb_v130; }
        { long long tb_v131 = (-1LL); long long *tb_p131 = &(((tb_o_Comp *)tb_nn(v_c, 603))->q_temp_base); *tb_p131 = tb_v131; }
        { long long tb_v132 = 0LL; long long *tb_p132 = &(((tb_o_Comp *)tb_nn(v_c, 604))->q_piece_depth); *tb_p132 = tb_v132; }
        { long long tb_v133 = (((tb_o_Comp *)tb_nn(v_c, 605))->q_vars)->len; long long *tb_p133 = &(((tb_o_Comp *)tb_nn(v_c, 605))->q_var_base); *tb_p133 = tb_v133; }
        f_check__check_func(v_c, v_fi);
        { long long tb_v134 = v_saved_pieces; long long *tb_p134 = &(((tb_o_Comp *)tb_nn(v_c, 607))->q_piece_depth); *tb_p134 = tb_v134; }
        { long long tb_v135 = v_saved_par; long long *tb_p135 = &(((tb_o_Comp *)tb_nn(v_c, 608))->q_par_base); *tb_p135 = tb_v135; }
        { long long tb_v136 = v_saved_temp; long long *tb_p136 = &(((tb_o_Comp *)tb_nn(v_c, 609))->q_temp_base); *tb_p136 = tb_v136; }
        { long long tb_v137 = v_saved_temp_loops; long long *tb_p137 = &(((tb_o_Comp *)tb_nn(v_c, 610))->q_temp_loops); *tb_p137 = tb_v137; }
        { long long tb_v138 = v_saved_base; long long *tb_p138 = &(((tb_o_Comp *)tb_nn(v_c, 611))->q_var_base); *tb_p138 = tb_v138; }
        { const char * tb_v139 = v_saved_mod; const char * *tb_p139 = &(((tb_o_Comp *)tb_nn(v_c, 612))->q_cur_mod); *tb_p139 = tb_v139; }
        { const char * tb_v140 = v_saved_file; const char * *tb_p140 = &(((tb_o_Comp *)tb_nn(v_c, 613))->q_file_name); *tb_p140 = tb_v140; }
        { long long tb_v141 = v_saved_current; long long *tb_p141 = &(((tb_o_Comp *)tb_nn(v_c, 614))->q_current); *tb_p141 = tb_v141; }
        { long long tb_v142 = v_saved_loops; long long *tb_p142 = &(((tb_o_Comp *)tb_nn(v_c, 615))->q_loop_depth); *tb_p142 = tb_v142; }
        f_check__cut_vars(v_c, v_saved_vars);
        { long long tb_v143 = v_saved_depth; long long *tb_p143 = &(((tb_o_Comp *)tb_nn(v_c, 617))->q_depth); *tb_p143 = tb_v143; }
    }
    if ((((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 618))->q_node, 618))->q_ty < 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 619))->q_line, "функция вызывает сама себя раньше первого return со значением: поставь простой return выше", ((tb_o_Node *)tb_nn(v_n, 619))->q_name);
    }
    return ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 620))->q_node, 620))->q_ty;
    return v_r;
}

static long long f_check__check_module_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    const char * v_mod = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 624))->q_kids, 0LL, sizeof(tb_o_Node *), 624)), 624))->q_name;
    if ((!f_check__module_uses(v_c, ((tb_o_Comp *)tb_nn(v_c, 625))->q_cur_mod, v_mod))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 626))->q_line, "этот модуль не подключён: добавь в начало файла  use", v_mod);
    }
    long long v_f = f_check__find_func_in(v_c, ((tb_o_Node *)tb_nn(v_n, 627))->q_name, v_mod);
    if ((v_f < 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 629))->q_line, "в этом модуле нет такой функции", ((tb_o_Node *)tb_nn(v_n, 629))->q_name);
    }
    if (((!((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 630))->q_funcs, v_f, sizeof(tb_o_Func *), 630)), 630))->q_node, 630))->q_is_pub) && (strcmp(v_mod, ((tb_o_Comp *)tb_nn(v_c, 630))->q_cur_mod) != 0))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 631))->q_line, "эта функция спрятана в своём модуле: поставь перед ней modpub", ((tb_o_Node *)tb_nn(v_n, 631))->q_name);
    }
    { const char * tb_v144 = "call"; const char * *tb_p144 = &(((tb_o_Node *)tb_nn(v_n, 633))->q_kind); *tb_p144 = tb_v144; }
    tb_list * v_rest = tb_list_new(sizeof(tb_o_Node *));
    {
        long long tb_end145 = ((((tb_o_Node *)tb_nn(v_n, 635))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end145; v_i++) {
            { tb_o_Node * tb_v146 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 636))->q_kids, v_i, sizeof(tb_o_Node *), 636)); *(tb_o_Node * *)tb_grow(v_rest) = tb_v146; }
        }
    }
    { tb_list * tb_v147 = v_rest; tb_list * *tb_p147 = &(((tb_o_Node *)tb_nn(v_n, 637))->q_kids); *tb_p147 = tb_v147; }
    return f_check__check_user_call(v_c, v_n, v_f);
    return v_r;
}

static long long f_check__check_dict_method(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t) {
    long long v_r = 0LL;
    long long v_args = ((((tb_o_Node *)tb_nn(v_n, 641))->q_kids)->len - 1LL);
    tb_o_Type * v_d = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 642))->q_types, v_t, sizeof(tb_o_Type *), 642));
    if (((strcmp(((tb_o_Node *)tb_nn(v_n, 643))->q_name, "has") == 0) || (strcmp(((tb_o_Node *)tb_nn(v_n, 643))->q_name, "remove") == 0))) {
        if ((v_args != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 645))->q_line, "нужен ровно один ключ", ((tb_o_Node *)tb_nn(v_n, 645))->q_name);
        }
        long long v_k = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 646))->q_kids, 1LL, sizeof(tb_o_Node *), 646)), 646))->q_kids, 0LL, sizeof(tb_o_Node *), 646)));
        if ((v_k != ((tb_o_Type *)tb_nn(v_d, 647))->q_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 648))->q_line, "ключ у этого dict другого типа", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_d, 648))->q_key));
        }
        { long long tb_v148 = ((tb_o_Type *)tb_nn(v_d, 649))->q_key; long long *tb_p148 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 649))->q_kids, 1LL, sizeof(tb_o_Node *), 649)), 649))->q_ty); *tb_p148 = tb_v148; }
        if ((strcmp(((tb_o_Node *)tb_nn(v_n, 650))->q_name, "has") == 0)) {
            return 3LL;
        }
        return 0LL;
    }
    if (((strcmp(((tb_o_Node *)tb_nn(v_n, 653))->q_name, "keys") == 0) || (strcmp(((tb_o_Node *)tb_nn(v_n, 653))->q_name, "values") == 0))) {
        if ((v_args != 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 655))->q_line, "пишется без значений", ((tb_o_Node *)tb_nn(v_n, 655))->q_name);
        }
        if ((strcmp(((tb_o_Node *)tb_nn(v_n, 656))->q_name, "keys") == 0)) {
            return f_check__list_of(v_c, ((tb_o_Type *)tb_nn(v_d, 657))->q_key);
        }
        return f_check__list_of(v_c, ((tb_o_Type *)tb_nn(v_d, 658))->q_elem);
    }
    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 659))->q_line, "у dict нет такого метода (есть .has .remove .keys .values)", ((tb_o_Node *)tb_nn(v_n, 659))->q_name);
    return (-1LL);
    return v_r;
}

static long long f_check__check_method(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    tb_o_Node * v_base = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 663))->q_kids, 0LL, sizeof(tb_o_Node *), 663));
    if ((((strcmp(((tb_o_Node *)tb_nn(v_base, 664))->q_kind, "name") == 0) && (f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_base, 664))->q_name) < 0LL)) && f_check__is_module(v_c, ((tb_o_Node *)tb_nn(v_base, 664))->q_name))) {
        return f_check__check_module_call(v_c, v_n);
    }
    long long v_t = f_check__check_value(v_c, v_base);
    long long v_args = ((((tb_o_Node *)tb_nn(v_n, 667))->q_kids)->len - 1LL);
    {
        long long tb_end149 = ((((tb_o_Node *)tb_nn(v_n, 668))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end149; v_i++) {
            if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 669))->q_kids, v_i, sizeof(tb_o_Node *), 669)), 669))->q_name, "") != 0)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 670))->q_line, "у методов аргументы пишутся без имён", "");
            }
        }
    }
    if ((((strcmp(((tb_o_Node *)tb_nn(v_n, 671))->q_name, "add") == 0) || (strcmp(((tb_o_Node *)tb_nn(v_n, 671))->q_name, "pop") == 0)) || (strcmp(((tb_o_Node *)tb_nn(v_n, 671))->q_name, "remove") == 0))) {
        f_check__no_outside_change(v_c, v_base);
    }
    if (f_check__is_dict(v_c, v_t)) {
        return f_check__check_dict_method(v_c, v_n, v_t);
    }
    if ((!f_check__is_list(v_c, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 676))->q_line, "у этого значения нет методов: они есть у списков (.add .pop) и dict (.has .remove .keys .values)", ((tb_o_Node *)tb_nn(v_n, 676))->q_name);
    }
    long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 677))->q_types, v_t, sizeof(tb_o_Type *), 677)), 677))->q_elem;
    if ((strcmp(((tb_o_Node *)tb_nn(v_n, 678))->q_name, "add") == 0)) {
        if ((v_args != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 680))->q_line, ".add кладёт ровно одно значение", "");
        }
        long long v_v = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 681))->q_kids, 1LL, sizeof(tb_o_Node *), 681)), 681))->q_kids, 0LL, sizeof(tb_o_Node *), 681)));
        if ((!f_check__fits(v_elem, v_v))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 683))->q_line, "в этот список кладут другой тип", f_check__type_name(v_c, v_elem));
        }
        { long long tb_v150 = v_elem; long long *tb_p150 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 684))->q_kids, 1LL, sizeof(tb_o_Node *), 684)), 684))->q_ty); *tb_p150 = tb_v150; }
        return 0LL;
    }
    if ((strcmp(((tb_o_Node *)tb_nn(v_n, 686))->q_name, "pop") == 0)) {
        if (f_check__is_cols_obj(v_c, v_elem)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 688))->q_line, "у списка колонок (by columns) .pop() пока нет", "");
        }
        if ((v_args != 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 690))->q_line, ".pop() пишется без значений", "");
        }
        return v_elem;
    }
    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 692))->q_line, "у списка нет такого метода (есть .add и .pop)", ((tb_o_Node *)tb_nn(v_n, 692))->q_name);
    return (-1LL);
    return v_r;
}

static long long f_check__check_dict(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    if (((((tb_o_Node *)tb_nn(v_n, 696))->q_kids)->len == 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 697))->q_line, "пустой dict пишется с типом: d = dict text int", "");
    }
    long long v_key = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 698))->q_kids, 0LL, sizeof(tb_o_Node *), 698)));
    long long v_value = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 699))->q_kids, 1LL, sizeof(tb_o_Node *), 699)));
    if (((v_key != 4LL) && (v_key != 1LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 701))->q_line, "ключ у dict бывает text или int", "");
    }
    long long v_i = 2LL;
    while (((v_i + 1LL) < (((tb_o_Node *)tb_nn(v_n, 703))->q_kids)->len)) {
        if ((f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 704))->q_kids, v_i, sizeof(tb_o_Node *), 704))) != v_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 705))->q_kids, v_i, sizeof(tb_o_Node *), 705)), 705))->q_line, "все ключи одного типа", f_check__type_name(v_c, v_key));
        }
        long long v_v = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 706))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 706)));
        if ((!f_check__fits(v_value, v_v))) {
            if (((v_value == 1LL) && (v_v == 2LL))) {
                v_value = 2LL;
            } else {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 711))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 711)), 711))->q_line, "все значения одного типа", f_check__type_name(v_c, v_value));
            }
        }
        { long long tb_v151 = 2LL; long long *tb_p151 = &(v_i); *tb_p151 += tb_v151; }
    }
    return f_check__dict_of(v_c, v_key, v_value);
    return v_r;
}

static _Bool f_check__can_fail_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if ((strcmp(((tb_o_Node *)tb_nn(v_n, 717))->q_kind, "call") == 0)) {
        if (f_check__is_builtin(((tb_o_Node *)tb_nn(v_n, 718))->q_name)) {
            return f_check__builtin_can_fail(((tb_o_Node *)tb_nn(v_n, 719))->q_name);
        }
        if (((f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 720))->q_name) >= 0LL) || (f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_n, 720))->q_name) >= 0LL))) {
            return 0;
        }
        long long v_f = f_check__find_func(v_c, ((tb_o_Node *)tb_nn(v_n, 722))->q_name);
        return ((v_f >= 0LL) && ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 723))->q_funcs, v_f, sizeof(tb_o_Func *), 723)), 723))->q_node, 723))->q_can_fail);
    }
    if (((((strcmp(((tb_o_Node *)tb_nn(v_n, 724))->q_kind, "method") == 0) && (strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 724))->q_kids, 0LL, sizeof(tb_o_Node *), 724)), 724))->q_kind, "name") == 0)) && (f_check__find_var(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 724))->q_kids, 0LL, sizeof(tb_o_Node *), 724)), 724))->q_name) < 0LL)) && f_check__is_module(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 724))->q_kids, 0LL, sizeof(tb_o_Node *), 724)), 724))->q_name))) {
        long long v_f = f_check__find_func_in(v_c, ((tb_o_Node *)tb_nn(v_n, 725))->q_name, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 725))->q_kids, 0LL, sizeof(tb_o_Node *), 725)), 725))->q_name);
        return ((v_f >= 0LL) && ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 726))->q_funcs, v_f, sizeof(tb_o_Func *), 726)), 726))->q_node, 726))->q_can_fail);
    }
    return 0;
    tb_crash(716, "функция can_fail_call дошла до конца без return");
    return 0;
}

static long long f_check__check_handled(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_how) {
    long long v_r = 0LL;
    if ((!f_check__can_fail_call(v_c, v_n))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 732))->q_line, "этот вызов не может закончиться ошибкой, здесь не нужно", v_how);
    }
    { _Bool tb_v152 = 1; _Bool *tb_p152 = &(((tb_o_Node *)tb_nn(v_n, 733))->q_creates); *tb_p152 = tb_v152; }
    return f_check__check_expr(v_c, v_n);
    return v_r;
}

static long long f_check__check_expr(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    long long v_t = (-1LL);
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 738))->q_kind;
    if ((strcmp(v_k, "number") == 0)) {
        v_t = 1LL;
    } else if ((strcmp(v_k, "decimal") == 0)) {
        v_t = 2LL;
    } else if ((strcmp(v_k, "string") == 0)) {
        v_t = 4LL;
    } else if ((strcmp(v_k, "bool") == 0)) {
        v_t = 3LL;
    } else if ((strcmp(v_k, "name") == 0)) {
        long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 748))->q_name);
        if ((v_v < 0LL)) {
            if ((f_check__find_func(v_c, ((tb_o_Node *)tb_nn(v_n, 750))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 751))->q_line, "у функции нужны скобки", ((tb_o_Node *)tb_nn(v_n, 751))->q_name);
            }
            if ((f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_n, 752))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 753))->q_line, "чтобы создать obj, нужны скобки", tb_cat(((tb_o_Node *)tb_nn(v_n, 753))->q_name, "(...)"));
            }
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 754))->q_line, "нет такой переменной", ((tb_o_Node *)tb_nn(v_n, 754))->q_name);
        }
        if ((f_check__outside_temp(v_c, v_v) && (!f_check__temp_can_see(v_c, ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 755))->q_vars, v_v, sizeof(tb_o_Var *), 755)), 755))->q_type)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 756))->q_line, "внутри temp не видно списков, dict и obj снаружи (кроме списков чисел): создай данные внутри temp или передай числа", ((tb_o_Node *)tb_nn(v_n, 756))->q_name);
        }
        if (f_check__outside_par(v_c, v_v)) {
            if ((((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 758))->q_vars, v_v, sizeof(tb_o_Var *), 758)), 758))->q_is_shared && (!((tb_o_Comp *)tb_nn(v_c, 758))->q_shared_ok))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 759))->q_line, "shared внутри on all cores можно только менять через += или -=, а читать — после цикла", ((tb_o_Node *)tb_nn(v_n, 759))->q_name);
            }
            if ((!(((tb_o_Comp *)tb_nn(v_c, 760))->q_par_vec && ((tb_o_Comp *)tb_nn(v_c, 760))->q_shared_ok))) {
                f_check__capture(v_c, v_v);
            }
        }
        { _Bool tb_v153 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 762))->q_vars, v_v, sizeof(tb_o_Var *), 762)), 762))->q_is_link; _Bool *tb_p153 = &(((tb_o_Node *)tb_nn(v_n, 762))->q_is_link); *tb_p153 = tb_v153; }
        v_t = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 763))->q_vars, v_v, sizeof(tb_o_Var *), 763)), 763))->q_type;
    } else if ((strcmp(v_k, "list") == 0)) {
        v_t = f_check__check_list(v_c, v_n);
    } else if ((strcmp(v_k, "dict") == 0)) {
        v_t = f_check__check_dict(v_c, v_n);
    } else if ((strcmp(v_k, "try") == 0)) {
        if ((((tb_o_Comp *)tb_nn(v_c, 769))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 770))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " try нельзя: ядро не может остановить всю функцию, пиши  or  или  else err"), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 771))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 772))->q_line, "внутри temp try нельзя: пиши  or  или  else err", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 773))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 774))->q_line, "внутри each ... by N try нельзя: пиши  or  или  else err", "");
        }
        v_t = f_check__check_handled(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 775))->q_kids, 0LL, sizeof(tb_o_Node *), 775)), "try");
    } else if ((strcmp(v_k, "else_err") == 0)) {
        v_t = f_check__check_handled(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 777))->q_kids, 0LL, sizeof(tb_o_Node *), 777)), "else");
        f_check__open_scope(v_c);
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 779))->q_name, 4LL, 1, 0, ((tb_o_Node *)tb_nn(v_n, 779))->q_line);
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 780))->q_kids, 1LL, sizeof(tb_o_Node *), 780)));
        f_check__close_scope(v_c);
    } else if ((strcmp(v_k, "call") == 0)) {
        v_t = f_check__check_call(v_c, v_n);
    } else if ((strcmp(v_k, "method") == 0)) {
        v_t = f_check__check_method(v_c, v_n);
    } else if ((strcmp(v_k, "binop") == 0)) {
        v_t = f_check__check_binop(v_c, v_n);
    } else if ((strcmp(v_k, "unary") == 0)) {
        long long v_inner = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 789))->q_kids, 0LL, sizeof(tb_o_Node *), 789)));
        if ((strcmp(((tb_o_Node *)tb_nn(v_n, 790))->q_value, "not") == 0)) {
            if ((v_inner != 3LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 792))->q_line, "not работает только с true и false", "");
            }
            v_t = 3LL;
        } else {
            if ((!f_check__is_number(v_inner))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 796))->q_line, "минус ставится только перед числом", "");
            }
            v_t = v_inner;
        }
    } else if (((strcmp(v_k, "index") == 0) && f_check__is_dict(v_c, f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 798))->q_kids, 0LL, sizeof(tb_o_Node *), 798)))))) {
        tb_o_Type * v_base = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 799))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 799))->q_kids, 0LL, sizeof(tb_o_Node *), 799)), 799))->q_ty, sizeof(tb_o_Type *), 799));
        if ((f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 800))->q_kids, 1LL, sizeof(tb_o_Node *), 800))) != ((tb_o_Type *)tb_nn(v_base, 800))->q_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 801))->q_line, "ключ у этого dict другого типа", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_base, 801))->q_key));
        }
        v_t = ((tb_o_Type *)tb_nn(v_base, 802))->q_elem;
    } else if ((strcmp(v_k, "index") == 0)) {
        long long v_base = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 804))->q_kids, 0LL, sizeof(tb_o_Node *), 804)));
        long long v_index = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 805))->q_kids, 1LL, sizeof(tb_o_Node *), 805)));
        if ((v_index != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 807))->q_line, "номер в списке должен быть int", "");
        }
        if ((v_base == 4LL)) {
            v_t = 4LL;
        } else if (f_check__is_list(v_c, v_base)) {
            v_t = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 811))->q_types, v_base, sizeof(tb_o_Type *), 811)), 811))->q_elem;
        } else {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 813))->q_line, "[номер] берут у списка или текста", "");
        }
    } else if ((((strcmp(v_k, "field") == 0) && (strcmp(((tb_o_Node *)tb_nn(v_n, 814))->q_name, "wait") == 0)) && (strcmp(f_check__kind_of(v_c, f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 814))->q_kids, 0LL, sizeof(tb_o_Node *), 814)))), "task") == 0))) {
        if ((((tb_o_Comp *)tb_nn(v_c, 815))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 816))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " .wait нельзя: забери готовое до цикла или после"), "");
        }
        v_t = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 817))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 817))->q_kids, 0LL, sizeof(tb_o_Node *), 817)), 817))->q_ty, sizeof(tb_o_Type *), 817)), 817))->q_elem;
    } else if ((strcmp(v_k, "background") == 0)) {
        v_t = f_check__check_background(v_c, v_n);
    } else if ((strcmp(v_k, "field") == 0)) {
        long long v_base = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 821))->q_kids, 0LL, sizeof(tb_o_Node *), 821)));
        if ((strcmp(f_check__kind_of(v_c, v_base), "obj") != 0)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 823))->q_line, "точка с качеством бывает только у obj", ((tb_o_Node *)tb_nn(v_n, 823))->q_name);
        }
        tb_o_Node * v_o = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 824))->q_types, v_base, sizeof(tb_o_Type *), 824)), 824))->q_obj_node;
        long long v_field = f_check__find_field(v_o, ((tb_o_Node *)tb_nn(v_n, 825))->q_name);
        if ((v_field < 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 827))->q_line, "у этого obj нет такого качества", ((tb_o_Node *)tb_nn(v_n, 827))->q_name);
        }
        v_t = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 828))->q_kids, v_field, sizeof(tb_o_Node *), 828)), 828))->q_ty;
    }
    { long long tb_v154 = v_t; long long *tb_p154 = &(((tb_o_Node *)tb_nn(v_n, 829))->q_ty); *tb_p154 = tb_v154; }
    return v_t;
    return v_r;
}

static void f_check__need_bool(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if ((f_check__check_value(v_c, v_n) != 3LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 834))->q_line, "условие должно быть true или false (например a > 3)", "");
    }
}

static void f_check__need_int(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_what) {
    if ((f_check__check_value(v_c, v_n) != 1LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 838))->q_line, "здесь нужно целое число int", v_what);
    }
}

static void f_check__check_assign(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_declared = (-1LL);
    if (((tb_o_Node *)tb_nn(v_n, 844))->q_has_tsyn) {
        v_declared = f_check__resolve_type(v_c, ((tb_o_Node *)tb_nn(v_n, 845))->q_tsyn);
    }
    long long v_t = (-1LL);
    if (((((tb_o_Node *)tb_nn(v_n, 847))->q_kids)->len == 1LL)) {
        v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 848))->q_kids, 0LL, sizeof(tb_o_Node *), 848)));
    }
    long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 849))->q_name);
    if (((tb_o_Node *)tb_nn(v_n, 850))->q_is_shared) {
        if ((v_v >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 852))->q_line, "shared пишется только у новой переменной", ((tb_o_Node *)tb_nn(v_n, 852))->q_name);
        }
        if (((((v_declared != 1LL) && (v_declared != 2LL)) && (v_t != 1LL)) && (v_t != 2LL))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 854))->q_line, "shared бывает int или dec: shared y = int 0", ((tb_o_Node *)tb_nn(v_n, 854))->q_name);
        }
    }
    if ((v_v < 0LL)) {
        long long v_type = v_t;
        if ((v_declared >= 0LL)) {
            if (((v_t >= 0LL) && (!f_check__fits(v_declared, v_t)))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 859))->q_line, "значение не того типа, что написан", f_check__type_name(v_c, v_declared));
            }
            v_type = v_declared;
        }
        { _Bool tb_v155 = 1; _Bool *tb_p155 = &(((tb_o_Node *)tb_nn(v_n, 861))->q_creates); *tb_p155 = tb_v155; }
        { long long tb_v156 = v_type; long long *tb_p156 = &(((tb_o_Node *)tb_nn(v_n, 862))->q_ty); *tb_p156 = tb_v156; }
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 863))->q_name, v_type, ((tb_o_Node *)tb_nn(v_n, 863))->q_is_const, 0, ((tb_o_Node *)tb_nn(v_n, 863))->q_line);
        { _Bool tb_v157 = ((tb_o_Node *)tb_nn(v_n, 864))->q_is_shared; _Bool *tb_p157 = &(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 864))->q_vars, ((((tb_o_Comp *)tb_nn(v_c, 864))->q_vars)->len - 1LL), sizeof(tb_o_Var *), 864)), 864))->q_is_shared); *tb_p157 = tb_v157; }
        return;
    }
    if ((f_check__outside_temp(v_c, v_v) && (!f_check__is_plain(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 866))->q_vars, v_v, sizeof(tb_o_Var *), 866)), 866))->q_type)))) {
        f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_n, 867))->q_line, ((tb_o_Node *)tb_nn(v_n, 867))->q_name);
    }
    if (f_check__outside_par(v_c, v_v)) {
        if (((tb_o_Comp *)tb_nn(v_c, 869))->q_par_vec) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 870))->q_line, "внутри by vec нельзя менять переменную снаружи: копи её через += или -=", ((tb_o_Node *)tb_nn(v_n, 870))->q_name);
        }
        if (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 871))->q_vars, v_v, sizeof(tb_o_Var *), 871)), 871))->q_is_shared) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 872))->q_line, "shared внутри on all cores меняется только через += или -=", ((tb_o_Node *)tb_nn(v_n, 872))->q_name);
        }
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 873))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="), ((tb_o_Node *)tb_nn(v_n, 873))->q_name);
    }
    if ((((tb_o_Node *)tb_nn(v_n, 874))->q_is_const || (v_declared >= 0LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 875))->q_line, "переменная уже есть: тип и const пишутся только в первый раз", ((tb_o_Node *)tb_nn(v_n, 875))->q_name);
    }
    if (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 876))->q_vars, v_v, sizeof(tb_o_Var *), 876)), 876))->q_is_const) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 877))->q_line, "это const или переменная цикла, её нельзя менять", ((tb_o_Node *)tb_nn(v_n, 877))->q_name);
    }
    if ((!f_check__fits(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 878))->q_vars, v_v, sizeof(tb_o_Var *), 878)), 878))->q_type, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 879))->q_line, "переменная другого типа", f_check__type_name(v_c, ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 879))->q_vars, v_v, sizeof(tb_o_Var *), 879)), 879))->q_type));
    }
    { _Bool tb_v158 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 880))->q_vars, v_v, sizeof(tb_o_Var *), 880)), 880))->q_is_link; _Bool *tb_p158 = &(((tb_o_Node *)tb_nn(v_n, 880))->q_is_link); *tb_p158 = tb_v158; }
    { long long tb_v159 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 881))->q_vars, v_v, sizeof(tb_o_Var *), 881)), 881))->q_type; long long *tb_p159 = &(((tb_o_Node *)tb_nn(v_n, 881))->q_ty); *tb_p159 = tb_v159; }
}

static void f_check__check_set(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 884))->q_kids, 0LL, sizeof(tb_o_Node *), 884));
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 885))->q_value;
    { _Bool tb_v160 = ((strcmp(((tb_o_Node *)tb_nn(v_target, 886))->q_kind, "name") == 0) && ((strcmp(v_op, "+=") == 0) || (strcmp(v_op, "-=") == 0))); _Bool *tb_p160 = &(((tb_o_Comp *)tb_nn(v_c, 886))->q_shared_ok); *tb_p160 = tb_v160; }
    long long v_to_ty = f_check__check_value(v_c, v_target);
    { _Bool tb_v161 = 0; _Bool *tb_p161 = &(((tb_o_Comp *)tb_nn(v_c, 888))->q_shared_ok); *tb_p161 = tb_v161; }
    f_check__need_place(v_c, v_target, "менять можно только переменную, u.age или nums[i]");
    long long v_r = f_check__root_var(v_c, v_target);
    if (((f_check__outside_temp(v_c, v_r) && (strcmp(((tb_o_Node *)tb_nn(v_target, 891))->q_kind, "name") == 0)) && (!f_check__is_plain(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 891))->q_vars, v_r, sizeof(tb_o_Var *), 891)), 891))->q_type)))) {
        f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_n, 892))->q_line, ((tb_o_Node *)tb_nn(v_target, 892))->q_name);
    }
    if ((f_check__outside_par(v_c, v_r) && ((tb_o_Comp *)tb_nn(v_c, 893))->q_par_vec)) {
        if ((strcmp(((tb_o_Node *)tb_nn(v_target, 894))->q_kind, "name") != 0)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 895))->q_line, "внутри by vec нельзя менять списки и obj снаружи цикла", "");
        }
        tb_o_Var * v_var = (*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 896))->q_vars, v_r, sizeof(tb_o_Var *), 896));
        if ((((strcmp(v_op, "+=") != 0) && (strcmp(v_op, "-=") != 0)) || (!f_check__is_number(((tb_o_Var *)tb_nn(v_var, 897))->q_type)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 898))->q_line, "внутри by vec число снаружи можно только копить через += или -=", ((tb_o_Node *)tb_nn(v_target, 898))->q_name);
        }
        _Bool v_found = 0;
        {
            tb_list * tb_l162 = ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 900))->q_par_node, 900))->q_caps;
            for (long long tb_i162 = 0; tb_i162 < tb_l162->len; tb_i162++) {
                tb_o_Var * const v_old = ((tb_o_Var * *)tb_l162->data)[tb_i162];
                if (((strcmp(((tb_o_Var *)tb_nn(v_old, 901))->q_name, ((tb_o_Var *)tb_nn(v_var, 901))->q_name) == 0) && ((tb_o_Var *)tb_nn(v_old, 901))->q_is_shared)) {
                    v_found = 1;
                    if ((strcmp(((tb_o_Var *)tb_nn(v_old, 903))->q_op, v_op) != 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 904))->q_line, "в by vec одну переменную копят одним знаком: или +=, или -=", ((tb_o_Node *)tb_nn(v_target, 904))->q_name);
                    }
                }
            }
        }
        if ((!v_found)) {
            { tb_o_Var * tb_v163 = tb_new_Var(((tb_o_Var *)tb_nn(v_var, 906))->q_name, ((tb_o_Var *)tb_nn(v_var, 906))->q_type, 0, ((tb_o_Var *)tb_nn(v_var, 906))->q_is_link, 0LL, 1, v_op); *(tb_o_Var * *)tb_grow(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 906))->q_par_node, 906))->q_caps) = tb_v163; }
        }
    } else if (f_check__outside_par(v_c, v_r)) {
        if ((strcmp(((tb_o_Node *)tb_nn(v_target, 908))->q_kind, "name") != 0)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 909))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)"), "");
        }
        if ((!((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 910))->q_vars, v_r, sizeof(tb_o_Var *), 910)), 910))->q_is_shared)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 911))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="), ((tb_o_Node *)tb_nn(v_target, 911))->q_name);
        }
        if (((strcmp(v_op, "+=") != 0) && (strcmp(v_op, "-=") != 0))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 913))->q_line, "shared внутри on all cores меняется только через += или -=", ((tb_o_Node *)tb_nn(v_target, 913))->q_name);
        }
    }
    long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 914))->q_kids, 1LL, sizeof(tb_o_Node *), 914)));
    { long long tb_v164 = v_to_ty; long long *tb_p164 = &(((tb_o_Node *)tb_nn(v_n, 915))->q_ty); *tb_p164 = tb_v164; }
    if ((strcmp(v_op, "=") == 0)) {
        if ((!f_check__fits(v_to_ty, v_t))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 918))->q_line, "здесь лежит другой тип", f_check__type_name(v_c, v_to_ty));
        }
        return;
    }
    if (((v_to_ty == 4LL) && (strcmp(v_op, "+=") == 0))) {
        if ((!f_check__is_scalar(v_t))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 922))->q_line, "к тексту можно приписать только текст, число или bool", "");
        }
        return;
    }
    if (((!f_check__is_number(v_to_ty)) || (!f_check__is_number(v_t)))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 925))->q_line, "эта операция работает только с числами", v_op);
    }
    if (((v_to_ty == 1LL) && (v_t == 2LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 927))->q_line, "в int нельзя добавить дробь: сделай его dec", "");
    }
}

static void f_check__check_linkdecl(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 930))->q_kids, 0LL, sizeof(tb_o_Node *), 930));
    long long v_t = f_check__check_value(v_c, v_target);
    f_check__need_place(v_c, v_target, "ярлык link ведёт к переменной, u.age или nums[i]");
    f_check__no_outside_change(v_c, v_target);
    if ((f_check__outside_temp(v_c, f_check__root_var(v_c, v_target)) && (!f_check__is_plain(v_t)))) {
        f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_n, 935))->q_line, ((tb_o_Node *)tb_nn(v_n, 935))->q_name);
    }
    { long long tb_v165 = v_t; long long *tb_p165 = &(((tb_o_Node *)tb_nn(v_n, 936))->q_ty); *tb_p165 = tb_v165; }
    f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 937))->q_name, v_t, 0, 1, ((tb_o_Node *)tb_nn(v_n, 937))->q_line);
}

static void f_check__check_multi(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 940))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 940))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 940));
    tb_o_Node * v_call = v_value;
    if (((strcmp(((tb_o_Node *)tb_nn(v_value, 942))->q_kind, "try") == 0) || (strcmp(((tb_o_Node *)tb_nn(v_value, 942))->q_kind, "else_err") == 0))) {
        v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 943))->q_kids, 0LL, sizeof(tb_o_Node *), 943));
    }
    if (((strcmp(((tb_o_Node *)tb_nn(v_call, 944))->q_kind, "call") != 0) && (strcmp(((tb_o_Node *)tb_nn(v_call, 944))->q_kind, "method") != 0))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 945))->q_line, "справа должен быть вызов функции с gives", "");
    }
    long long v_t = f_check__check_expr(v_c, v_value);
    long long v_names = ((((tb_o_Node *)tb_nn(v_n, 947))->q_kids)->len - 1LL);
    if ((strcmp(f_check__kind_of(v_c, v_t), "multi") != 0)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 949))->q_line, "у этой функции один выход, пиши  x = ...", ((tb_o_Node *)tb_nn(v_call, 949))->q_name);
    }
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 950))->q_types, v_t, sizeof(tb_o_Type *), 950)), 950))->q_func_node, 950))->q_kids, 2LL, sizeof(tb_o_Node *), 950));
    if (((((tb_o_Node *)tb_nn(v_outputs, 951))->q_kids)->len != v_names)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 952))->q_line, "у функции другое число выходов", ((tb_o_Node *)tb_nn(v_call, 952))->q_name);
    }
    {
        long long tb_end166 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end166; v_i++) {
            tb_o_Node * v_name = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 954))->q_kids, v_i, sizeof(tb_o_Node *), 954));
            long long v_out = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 955))->q_kids, v_i, sizeof(tb_o_Node *), 955)), 955))->q_ty;
            long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_name, 956))->q_name);
            if ((v_v < 0LL)) {
                { _Bool tb_v167 = 1; _Bool *tb_p167 = &(((tb_o_Node *)tb_nn(v_name, 958))->q_creates); *tb_p167 = tb_v167; }
                f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_name, 959))->q_name, v_out, 0, 0, ((tb_o_Node *)tb_nn(v_name, 959))->q_line);
            } else {
                if (f_check__outside_par(v_c, v_v)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_name, 962))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="), ((tb_o_Node *)tb_nn(v_name, 962))->q_name);
                }
                if ((f_check__outside_temp(v_c, v_v) && (!f_check__is_plain(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 963))->q_vars, v_v, sizeof(tb_o_Var *), 963)), 963))->q_type)))) {
                    f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_name, 964))->q_line, ((tb_o_Node *)tb_nn(v_name, 964))->q_name);
                }
                if (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 965))->q_vars, v_v, sizeof(tb_o_Var *), 965)), 965))->q_is_const) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_name, 966))->q_line, "это const или переменная цикла, её нельзя менять", ((tb_o_Node *)tb_nn(v_name, 966))->q_name);
                }
                if ((!f_check__fits(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 967))->q_vars, v_v, sizeof(tb_o_Var *), 967)), 967))->q_type, v_out))) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_name, 968))->q_line, "переменная другого типа", f_check__type_name(v_c, ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 968))->q_vars, v_v, sizeof(tb_o_Var *), 968)), 968))->q_type));
                }
                { _Bool tb_v168 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 969))->q_vars, v_v, sizeof(tb_o_Var *), 969)), 969))->q_is_link; _Bool *tb_p168 = &(((tb_o_Node *)tb_nn(v_name, 969))->q_is_link); *tb_p168 = tb_v168; }
            }
            { long long tb_v169 = v_out; long long *tb_p169 = &(((tb_o_Node *)tb_nn(v_name, 970))->q_ty); *tb_p169 = tb_v169; }
        }
    }
}

static void f_check__check_return(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_f = ((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 973))->q_funcs, ((tb_o_Comp *)tb_nn(v_c, 973))->q_current, sizeof(tb_o_Func *), 973)), 973))->q_node;
    if (((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 974))->q_kids, 2LL, sizeof(tb_o_Node *), 974)), 974))->q_kids)->len > 0LL)) {
        if (((((tb_o_Node *)tb_nn(v_n, 975))->q_kids)->len != 0LL)) {
            if (((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 977))->q_kids, 2LL, sizeof(tb_o_Node *), 977)), 977))->q_kids)->len > 1LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 978))->q_line, "у функции с несколькими gives return пишется без значения: он отдаёт выходы", "");
            }
            long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 979))->q_kids, 0LL, sizeof(tb_o_Node *), 979)));
            if ((!f_check__fits(((tb_o_Node *)tb_nn(v_f, 980))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 981))->q_line, "return отдаёт другой тип, чем написано в gives", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_f, 981))->q_ty));
            }
        }
        return;
    }
    long long v_t = 0LL;
    if (((((tb_o_Node *)tb_nn(v_n, 984))->q_kids)->len == 1LL)) {
        v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 985))->q_kids, 0LL, sizeof(tb_o_Node *), 985)));
    }
    if (((strcmp(((tb_o_Node *)tb_nn(v_f, 986))->q_name, "main") == 0) && (v_t != 0LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 987))->q_line, "main ничего не возвращает: пиши просто return", "");
    }
    if ((((tb_o_Node *)tb_nn(v_f, 988))->q_ty < 0LL)) {
        { long long tb_v170 = v_t; long long *tb_p170 = &(((tb_o_Node *)tb_nn(v_f, 989))->q_ty); *tb_p170 = tb_v170; }
    } else if (((((tb_o_Node *)tb_nn(v_f, 990))->q_ty == 1LL) && (v_t == 2LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 991))->q_line, "раньше функция вернула int, а тут dec: сделай оба числа одного типа", "");
    } else if ((!f_check__fits(((tb_o_Node *)tb_nn(v_f, 992))->q_ty, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 993))->q_line, "раньше эта функция вернула другой тип", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_f, 993))->q_ty));
    }
}

static void f_check__check_statement(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 996))->q_kind;
    if ((strcmp(v_k, "assign") == 0)) {
        f_check__check_assign(v_c, v_n);
    } else if ((strcmp(v_k, "set") == 0)) {
        f_check__check_set(v_c, v_n);
    } else if ((strcmp(v_k, "linkdecl") == 0)) {
        f_check__check_linkdecl(v_c, v_n);
    } else if ((strcmp(v_k, "multi") == 0)) {
        f_check__check_multi(v_c, v_n);
    } else if ((strcmp(v_k, "if") == 0)) {
        long long v_i = 0LL;
        while (((v_i + 1LL) < (((tb_o_Node *)tb_nn(v_n, 1007))->q_kids)->len)) {
            f_check__need_bool(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1008))->q_kids, v_i, sizeof(tb_o_Node *), 1008)));
            f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1009))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 1009)));
            { long long tb_v171 = 2LL; long long *tb_p171 = &(v_i); *tb_p171 += tb_v171; }
        }
        if ((tb_mod((((tb_o_Node *)tb_nn(v_n, 1011))->q_kids)->len, 2LL, 1011) == 1LL)) {
            f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1012))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 1012))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 1012)));
        }
    } else if ((strcmp(v_k, "each") == 0)) {
        long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1014))->q_kids, 0LL, sizeof(tb_o_Node *), 1014)));
        long long v_elem = 4LL;
        if (f_check__is_list(v_c, v_t)) {
            v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1017))->q_types, v_t, sizeof(tb_o_Type *), 1017)), 1017))->q_elem;
        } else if (f_check__is_dict(v_c, v_t)) {
            v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1019))->q_types, v_t, sizeof(tb_o_Type *), 1019)), 1019))->q_key;
        } else if ((v_t != 4LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1021))->q_line, "each ходит по списку, по ключам dict или по буквам текста", "");
        }
        if ((((tb_o_Node *)tb_nn(v_n, 1022))->q_is_par && (!f_check__is_list(v_c, v_t)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1023))->q_line, "on all cores ходит по списку (или count from ... to ...)", "");
        }
        if ((((tb_o_Node *)tb_nn(v_n, 1024))->q_by_vec && (!(f_check__is_list(v_c, v_t) && f_check__is_number(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1024))->q_types, v_t, sizeof(tb_o_Type *), 1024)), 1024))->q_elem))))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1025))->q_line, "by vec ходит по списку чисел: list int или list dec", "");
        }
        if (((tb_o_Node *)tb_nn(v_n, 1026))->q_by_piece) {
            f_check__check_pieces(v_c, v_n, v_t);
        } else {
            f_check__open_scope(v_c);
            f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1030))->q_name, v_elem, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1030))->q_line);
            f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1031))->q_kids, 1LL, sizeof(tb_o_Node *), 1031)));
            f_check__close_scope(v_c);
        }
    } else if ((strcmp(v_k, "count") == 0)) {
        f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1034))->q_kids, 0LL, sizeof(tb_o_Node *), 1034)), "from");
        f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1035))->q_kids, 1LL, sizeof(tb_o_Node *), 1035)), "to");
        f_check__open_scope(v_c);
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1037))->q_name, 1LL, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1037))->q_line);
        f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1038))->q_kids, 2LL, sizeof(tb_o_Node *), 1038)));
        f_check__close_scope(v_c);
    } else if (((strcmp(v_k, "repeat_times") == 0) || (strcmp(v_k, "repeat_while") == 0))) {
        if ((strcmp(v_k, "repeat_times") == 0)) {
            f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1042))->q_kids, 0LL, sizeof(tb_o_Node *), 1042)), "times");
        } else {
            f_check__need_bool(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1044))->q_kids, 0LL, sizeof(tb_o_Node *), 1044)));
        }
        { long long tb_v172 = 1LL; long long *tb_p172 = &(((tb_o_Comp *)tb_nn(v_c, 1045))->q_loop_depth); *tb_p172 += tb_v172; }
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1046))->q_kids, 1LL, sizeof(tb_o_Node *), 1046)));
        { long long tb_v173 = 1LL; long long *tb_p173 = &(((tb_o_Comp *)tb_nn(v_c, 1047))->q_loop_depth); *tb_p173 -= tb_v173; }
    } else if ((strcmp(v_k, "together") == 0)) {
        f_check__check_together(v_c, v_n);
    } else if ((strcmp(v_k, "temp") == 0)) {
        long long v_saved_base = ((tb_o_Comp *)tb_nn(v_c, 1051))->q_temp_base;
        long long v_saved_loops = ((tb_o_Comp *)tb_nn(v_c, 1052))->q_temp_loops;
        { long long tb_v174 = (((tb_o_Comp *)tb_nn(v_c, 1053))->q_vars)->len; long long *tb_p174 = &(((tb_o_Comp *)tb_nn(v_c, 1053))->q_temp_base); *tb_p174 = tb_v174; }
        { long long tb_v175 = ((tb_o_Comp *)tb_nn(v_c, 1054))->q_loop_depth; long long *tb_p175 = &(((tb_o_Comp *)tb_nn(v_c, 1054))->q_temp_loops); *tb_p175 = tb_v175; }
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1055))->q_kids, 0LL, sizeof(tb_o_Node *), 1055)));
        { long long tb_v176 = v_saved_base; long long *tb_p176 = &(((tb_o_Comp *)tb_nn(v_c, 1056))->q_temp_base); *tb_p176 = tb_v176; }
        { long long tb_v177 = v_saved_loops; long long *tb_p177 = &(((tb_o_Comp *)tb_nn(v_c, 1057))->q_temp_loops); *tb_p177 = tb_v177; }
    } else if (((strcmp(v_k, "skip") == 0) || (strcmp(v_k, "stop") == 0))) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1059))->q_loop_depth == 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1060))->q_line, "skip и stop пишутся только внутри цикла", "");
        }
        if ((((strcmp(v_k, "stop") == 0) && (((tb_o_Comp *)tb_nn(v_c, 1061))->q_par_base >= 0LL)) && (((tb_o_Comp *)tb_nn(v_c, 1061))->q_loop_depth == ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1061))->q_par_node, 1061))->q_temp_no))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1062))->q_line, "stop внутри on all cores нельзя: остальные ядра всё равно работают", "");
        }
        if (((((tb_o_Comp *)tb_nn(v_c, 1063))->q_temp_base >= 0LL) && (((tb_o_Comp *)tb_nn(v_c, 1063))->q_loop_depth == ((tb_o_Comp *)tb_nn(v_c, 1063))->q_temp_loops))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1064))->q_line, "skip и stop внутри temp работают только для циклов внутри temp", "");
        }
    } else if ((strcmp(v_k, "return") == 0)) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1066))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1067))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " return нельзя"), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1068))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1069))->q_line, "внутри temp return нельзя: сохрани число в переменную снаружи и выйди из блока", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1070))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1071))->q_line, "внутри each ... by N return нельзя: сохрани число в переменную снаружи и выйди через stop", "");
        }
        f_check__check_return(v_c, v_n);
    } else if ((strcmp(v_k, "fail") == 0)) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1074))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1075))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " fail нельзя: пиши  or  или  else err"), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1076))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1077))->q_line, "внутри temp fail нельзя: пиши  or  или  else err", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1078))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1079))->q_line, "внутри each ... by N fail нельзя: пиши  or  или  else err", "");
        }
        if ((f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1080))->q_kids, 0LL, sizeof(tb_o_Node *), 1080))) != 4LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1081))->q_line, "после fail пишется текст ошибки: fail \"нет файла\"", "");
        }
    } else if ((strcmp(v_k, "expr_stmt") == 0)) {
        tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1083))->q_kids, 0LL, sizeof(tb_o_Node *), 1083));
        tb_o_Node * v_inner = v_e;
        if (((strcmp(((tb_o_Node *)tb_nn(v_e, 1085))->q_kind, "try") == 0) || (strcmp(((tb_o_Node *)tb_nn(v_e, 1085))->q_kind, "else_err") == 0))) {
            v_inner = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1086))->q_kids, 0LL, sizeof(tb_o_Node *), 1086));
        }
        if ((((strcmp(((tb_o_Node *)tb_nn(v_inner, 1087))->q_kind, "call") != 0) && (strcmp(((tb_o_Node *)tb_nn(v_inner, 1087))->q_kind, "method") != 0)) && (!((strcmp(((tb_o_Node *)tb_nn(v_inner, 1087))->q_kind, "field") == 0) && (strcmp(((tb_o_Node *)tb_nn(v_inner, 1087))->q_name, "wait") == 0))))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1088))->q_line, "строка ничего не делает", "");
        }
        long long v_t = f_check__check_expr(v_c, v_e);
        if ((v_t != 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1091))->q_line, "результат вызова пропадает: сохрани его, например x = ...", ((tb_o_Node *)tb_nn(v_e, 1091))->q_name);
        }
    }
}

static long long f_check__check_background(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    if ((((tb_o_Comp *)tb_nn(v_c, 1097))->q_par_base >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1098))->q_line, tb_cat(tb_cat("внутри ", f_check__loop_word(v_c)), " background нельзя: цикл и так идёт на всех ядрах"), "");
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1099))->q_temp_base >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1100))->q_line, "внутри temp background нельзя: память блока удалится, пока функция ещё работает", "");
    }
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1101))->q_kids, 0LL, sizeof(tb_o_Node *), 1101));
    if (((strcmp(((tb_o_Node *)tb_nn(v_call, 1102))->q_kind, "call") == 0) && f_check__is_builtin(((tb_o_Node *)tb_nn(v_call, 1102))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1103))->q_line, "background запускает свою функцию: a = background load(name)", ((tb_o_Node *)tb_nn(v_call, 1103))->q_name);
    }
    { _Bool tb_v178 = 1; _Bool *tb_p178 = &(((tb_o_Node *)tb_nn(v_call, 1104))->q_creates); *tb_p178 = tb_v178; }
    long long v_t = f_check__check_expr(v_c, v_call);
    if ((((strcmp(((tb_o_Node *)tb_nn(v_call, 1106))->q_kind, "call") != 0) || (!((tb_o_Node *)tb_nn(v_call, 1106))->q_has_ref)) || (strcmp(((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_call, 1106))->q_ref, 1106))->q_kind, "func") != 0))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1107))->q_line, "background запускает свою функцию: a = background load(name)", "");
    }
    tb_o_Node * v_f = ((tb_o_Node *)tb_nn(v_call, 1108))->q_ref;
    if (((tb_o_Node *)tb_nn(v_f, 1109))->q_can_fail) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1110))->q_line, "функция с fail в background пока нельзя: пусть она отдаёт текст ошибки обычным выходом", ((tb_o_Node *)tb_nn(v_f, 1110))->q_name);
    }
    if (((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1111))->q_kids, 2LL, sizeof(tb_o_Node *), 1111)), 1111))->q_kids)->len > 1LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1112))->q_line, "в background функция с несколькими выходами пока нельзя", ((tb_o_Node *)tb_nn(v_f, 1112))->q_name);
    }
    {
        tb_list * tb_l179 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1113))->q_kids, 0LL, sizeof(tb_o_Node *), 1113)), 1113))->q_kids;
        for (long long tb_i179 = 0; tb_i179 < tb_l179->len; tb_i179++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l179->data)[tb_i179];
            if ((((tb_o_Node *)tb_nn(v_p, 1114))->q_is_link || (!(f_check__is_plain(((tb_o_Node *)tb_nn(v_p, 1114))->q_ty) || (((tb_o_Node *)tb_nn(v_p, 1114))->q_ty == 4LL))))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1115))->q_line, "в background функция получает только int, dec, bool и text: списки и obj могли бы меняться сразу из двух мест", ((tb_o_Node *)tb_nn(v_p, 1115))->q_name);
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
    tb_o_Node * v_block = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1123))->q_kids, 0LL, sizeof(tb_o_Node *), 1123));
    tb_list * v_names = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l180 = ((tb_o_Node *)tb_nn(v_block, 1125))->q_kids;
        for (long long tb_i180 = 0; tb_i180 < tb_l180->len; tb_i180++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l180->data)[tb_i180];
            if (((strcmp(((tb_o_Node *)tb_nn(v_s, 1126))->q_kind, "assign") != 0) || ((((tb_o_Node *)tb_nn(v_s, 1126))->q_kids)->len != 1LL))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1127))->q_line, "в together пишутся только новые переменные со значением: a = d[key]", "");
            }
            if ((f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_s, 1128))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1129))->q_line, "в together заводятся только новые переменные, а эта уже есть", ((tb_o_Node *)tb_nn(v_s, 1129))->q_name);
            }
            {
                tb_list * tb_l181 = v_names;
                for (long long tb_i181 = 0; tb_i181 < tb_l181->len; tb_i181++) {
                    const char * const v_w = ((const char * *)tb_l181->data)[tb_i181];
                    if ((strcmp(v_w, ((tb_o_Node *)tb_nn(v_s, 1131))->q_name) == 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1132))->q_line, "в together эта переменная уже заведена строкой выше", ((tb_o_Node *)tb_nn(v_s, 1132))->q_name);
                    }
                    if (f_check__uses_name((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_s, 1133))->q_kids, 0LL, sizeof(tb_o_Node *), 1133)), v_w)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1134))->q_line, "строки в together не должны зависеть друг от друга, а здесь нужна", v_w);
                    }
                }
            }
            { const char * tb_v182 = ((tb_o_Node *)tb_nn(v_s, 1135))->q_name; *(const char * *)tb_grow(v_names) = tb_v182; }
            f_check__check_statement(v_c, v_s);
        }
    }
}

static _Bool f_check__uses_name(tb_o_Node * v_n, const char * v_name) {
    if (((strcmp(((tb_o_Node *)tb_nn(v_n, 1140))->q_kind, "name") == 0) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1140))->q_name, v_name) == 0))) {
        return 1;
    }
    {
        tb_list * tb_l183 = ((tb_o_Node *)tb_nn(v_n, 1142))->q_kids;
        for (long long tb_i183 = 0; tb_i183 < tb_l183->len; tb_i183++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l183->data)[tb_i183];
            if (f_check__uses_name(v_k, v_name)) {
                return 1;
            }
        }
    }
    return 0;
    tb_crash(1139, "функция uses_name дошла до конца без return");
    return 0;
}

static void f_check__check_pieces(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t) {
    if ((!f_check__is_list(v_c, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1151))->q_line, "by N режет на куски список: each piece in nums by 64", "");
    }
    if (f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1152))->q_types, v_t, sizeof(tb_o_Type *), 1152)), 1152))->q_elem)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1153))->q_line, "список колонок (by columns) на куски пока не режется", "");
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1154))->q_par_base >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1155))->q_line, tb_cat(tb_cat("each ... by N внутри ", f_check__loop_word(v_c)), " пока нельзя"), "");
    }
    f_check__need_int(v_c, ((tb_o_Node *)tb_nn(v_n, 1156))->q_piece_size, "by");
    { const char * tb_v184 = ((tb_o_Comp *)tb_nn(v_c, 1157))->q_file_name; const char * *tb_p184 = &(((tb_o_Node *)tb_nn(v_n, 1157))->q_file); *tb_p184 = tb_v184; }
    { tb_o_Node * tb_v185 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1158))->q_pieces) = tb_v185; }
    f_check__open_scope(v_c);
    f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1160))->q_name, v_t, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1160))->q_line);
    { long long tb_v186 = 1LL; long long *tb_p186 = &(((tb_o_Comp *)tb_nn(v_c, 1161))->q_piece_depth); *tb_p186 += tb_v186; }
    f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1162))->q_kids, 1LL, sizeof(tb_o_Node *), 1162)));
    { long long tb_v187 = 1LL; long long *tb_p187 = &(((tb_o_Comp *)tb_nn(v_c, 1163))->q_piece_depth); *tb_p187 -= tb_v187; }
    f_check__close_scope(v_c);
}

static _Bool f_check__is_name_of(tb_o_Node * v_v, const char * v_name) {
    return ((strcmp(((tb_o_Node *)tb_nn(v_v, 1168))->q_kind, "name") == 0) && (strcmp(((tb_o_Node *)tb_nn(v_v, 1168))->q_name, v_name) == 0));
    tb_crash(1167, "функция is_name_of дошла до конца без return");
    return 0;
}

static long long f_check__piece_leak(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_name) {
    long long v_r = 0LL;
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1174))->q_kind;
    if (((strcmp(v_k, "name") == 0) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1175))->q_name, v_name) == 0))) {
        return ((tb_o_Node *)tb_nn(v_n, 1176))->q_line;
    }
    if ((((strcmp(v_k, "assign") == 0) || (strcmp(v_k, "linkdecl") == 0)) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1177))->q_name, v_name) == 0))) {
        return ((tb_o_Node *)tb_nn(v_n, 1178))->q_line;
    }
    if ((((strcmp(v_k, "call") == 0) && ((tb_o_Node *)tb_nn(v_n, 1179))->q_has_ref) && (strcmp(((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1179))->q_ref, 1179))->q_kind, "func") == 0))) {
        {
            long long tb_end188 = ((((tb_o_Node *)tb_nn(v_n, 1180))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end188; v_i++) {
                tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1181))->q_kids, v_i, sizeof(tb_o_Node *), 1181)), 1181))->q_kids, 0LL, sizeof(tb_o_Node *), 1181));
                if (f_check__is_name_of(v_v, v_name)) {
                    if ((!f_check__param_keeps_piece(v_c, ((tb_o_Node *)tb_nn(v_n, 1183))->q_ref, v_i))) {
                        continue;
                    }
                    return ((tb_o_Node *)tb_nn(v_v, 1185))->q_line;
                }
                long long v_bad = f_check__piece_leak(v_c, v_v, v_name);
                if ((v_bad != 0LL)) {
                    return v_bad;
                }
            }
        }
        return 0LL;
    }
    if (((strcmp(v_k, "call") == 0) && f_check__is_builtin(((tb_o_Node *)tb_nn(v_n, 1190))->q_name))) {
        {
            tb_list * tb_l189 = ((tb_o_Node *)tb_nn(v_n, 1191))->q_kids;
            for (long long tb_i189 = 0; tb_i189 < tb_l189->len; tb_i189++) {
                tb_o_Node * const v_a = ((tb_o_Node * *)tb_l189->data)[tb_i189];
                tb_o_Node * v_v = v_a;
                if ((strcmp(((tb_o_Node *)tb_nn(v_a, 1193))->q_kind, "arg") == 0)) {
                    v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_a, 1194))->q_kids, 0LL, sizeof(tb_o_Node *), 1194));
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
    if (((((strcmp(v_k, "index") == 0) || (strcmp(v_k, "each") == 0)) && ((((tb_o_Node *)tb_nn(v_n, 1201))->q_kids)->len > 0LL)) && f_check__is_name_of((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1201))->q_kids, 0LL, sizeof(tb_o_Node *), 1201)), v_name))) {
        v_start = 1LL;
    }
    if (((strcmp(v_k, "each") == 0) && ((tb_o_Node *)tb_nn(v_n, 1203))->q_by_piece)) {
        long long v_bad = f_check__piece_leak(v_c, ((tb_o_Node *)tb_nn(v_n, 1204))->q_piece_size, v_name);
        if ((v_bad != 0LL)) {
            return v_bad;
        }
    }
    {
        long long tb_end190 = ((((tb_o_Node *)tb_nn(v_n, 1207))->q_kids)->len - 1LL);
        for (long long v_i = v_start; v_i <= tb_end190; v_i++) {
            long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1208))->q_kids, v_i, sizeof(tb_o_Node *), 1208)), v_name);
            if ((v_bad != 0LL)) {
                return v_bad;
            }
        }
    }
    return 0LL;
    return v_r;
}

static _Bool f_check__param_keeps_piece(tb_o_Comp * v_c, tb_o_Node * v_f, long long v_i) {
    const char * v_key = tb_cat(tb_cat(tb_cat(tb_cat(((tb_o_Node *)tb_nn(v_f, 1215))->q_name, "@"), ((tb_o_Node *)tb_nn(v_f, 1215))->q_mod), "#"), tb_itos(v_i));
    {
        long long tb_end191 = ((((tb_o_Comp *)tb_nn(v_c, 1216))->q_safe_keys)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end191; v_j++) {
            if ((strcmp((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1217))->q_safe_keys, v_j, sizeof(const char *), 1217)), v_key) == 0)) {
                return ((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1218))->q_safe_vals, v_j, sizeof(long long), 1218)) == 2LL);
            }
        }
    }
    { const char * tb_v192 = v_key; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1220))->q_safe_keys) = tb_v192; }
    { long long tb_v193 = 1LL; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1221))->q_safe_vals) = tb_v193; }
    long long v_slot = ((((tb_o_Comp *)tb_nn(v_c, 1222))->q_safe_keys)->len - 1LL);
    { long long tb_v194 = 1LL; long long *tb_p194 = &(((tb_o_Comp *)tb_nn(v_c, 1223))->q_safe_busy); *tb_p194 += tb_v194; }
    long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1224))->q_kids, 1LL, sizeof(tb_o_Node *), 1224)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1224))->q_kids, 0LL, sizeof(tb_o_Node *), 1224)), 1224))->q_kids, v_i, sizeof(tb_o_Node *), 1224)), 1224))->q_name);
    { long long tb_v195 = 1LL; long long *tb_p195 = &(((tb_o_Comp *)tb_nn(v_c, 1225))->q_safe_busy); *tb_p195 -= tb_v195; }
    if ((v_bad != 0LL)) {
        { long long tb_v196 = 2LL; long long *tb_p196 = &((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1227))->q_safe_vals, v_slot, sizeof(long long), 1227))); *tb_p196 = tb_v196; }
        return 1;
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1229))->q_safe_busy > 0LL)) {
        { long long tb_v197 = 3LL; long long *tb_p197 = &((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1231))->q_safe_vals, v_slot, sizeof(long long), 1231))); *tb_p197 = tb_v197; }
        { const char * tb_v198 = ""; const char * *tb_p198 = &((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1232))->q_safe_keys, v_slot, sizeof(const char *), 1232))); *tb_p198 = tb_v198; }
    }
    return 0;
    tb_crash(1214, "функция param_keeps_piece дошла до конца без return");
    return 0;
}

static void f_check__check_piece_leaks(tb_o_Comp * v_c) {
    {
        tb_list * tb_l199 = ((tb_o_Comp *)tb_nn(v_c, 1237))->q_pieces;
        for (long long tb_i199 = 0; tb_i199 < tb_l199->len; tb_i199++) {
            tb_o_Node * const v_n = ((tb_o_Node * *)tb_l199->data)[tb_i199];
            long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1238))->q_kids, 1LL, sizeof(tb_o_Node *), 1238)), ((tb_o_Node *)tb_nn(v_n, 1238))->q_name);
            if ((v_bad != 0LL)) {
                { const char * tb_v200 = ((tb_o_Node *)tb_nn(v_n, 1240))->q_file; const char * *tb_p200 = &(((tb_o_Comp *)tb_nn(v_c, 1240))->q_file_name); *tb_p200 = tb_v200; }
                f_errors__error_at(v_c, v_bad, "кусок живёт только внутри своего цикла: его можно читать и менять по номеру, ходить по нему each и передавать в функцию, но нельзя сохранять, растить .add и уменьшать .pop", ((tb_o_Node *)tb_nn(v_n, 1241))->q_name);
            }
        }
    }
}

static void f_check__check_loop_body(tb_o_Comp * v_c, tb_o_Node * v_n, tb_o_Node * v_body) {
    { long long tb_v201 = 1LL; long long *tb_p201 = &(((tb_o_Comp *)tb_nn(v_c, 1246))->q_loop_depth); *tb_p201 += tb_v201; }
    if ((((tb_o_Node *)tb_nn(v_n, 1247))->q_is_par || ((tb_o_Node *)tb_nn(v_n, 1247))->q_by_vec)) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1248))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1249))->q_line, "on all cores и by vec внутри другого on all cores или by vec нельзя", "");
        }
        { long long tb_v202 = ((((tb_o_Comp *)tb_nn(v_c, 1250))->q_vars)->len - 1LL); long long *tb_p202 = &(((tb_o_Comp *)tb_nn(v_c, 1250))->q_par_base); *tb_p202 = tb_v202; }
        { tb_o_Node * tb_v203 = v_n; tb_o_Node * *tb_p203 = &(((tb_o_Comp *)tb_nn(v_c, 1251))->q_par_node); *tb_p203 = tb_v203; }
        { _Bool tb_v204 = ((tb_o_Node *)tb_nn(v_n, 1252))->q_by_vec; _Bool *tb_p204 = &(((tb_o_Comp *)tb_nn(v_c, 1252))->q_par_vec); *tb_p204 = tb_v204; }
        { long long tb_v205 = ((tb_o_Comp *)tb_nn(v_c, 1254))->q_loop_depth; long long *tb_p205 = &(((tb_o_Node *)tb_nn(v_n, 1254))->q_temp_no); *tb_p205 = tb_v205; }
        f_check__check_block(v_c, v_body);
        { long long tb_v206 = (-1LL); long long *tb_p206 = &(((tb_o_Comp *)tb_nn(v_c, 1256))->q_par_base); *tb_p206 = tb_v206; }
        { _Bool tb_v207 = 0; _Bool *tb_p207 = &(((tb_o_Comp *)tb_nn(v_c, 1257))->q_par_vec); *tb_p207 = tb_v207; }
        if (((tb_o_Node *)tb_nn(v_n, 1258))->q_by_vec) {
            {
                tb_list * tb_l208 = ((tb_o_Node *)tb_nn(v_n, 1260))->q_caps;
                for (long long tb_i208 = 0; tb_i208 < tb_l208->len; tb_i208++) {
                    tb_o_Var * const v_a = ((tb_o_Var * *)tb_l208->data)[tb_i208];
                    {
                        tb_list * tb_l209 = ((tb_o_Node *)tb_nn(v_n, 1261))->q_caps;
                        for (long long tb_i209 = 0; tb_i209 < tb_l209->len; tb_i209++) {
                            tb_o_Var * const v_b = ((tb_o_Var * *)tb_l209->data)[tb_i209];
                            if ((((strcmp(((tb_o_Var *)tb_nn(v_a, 1262))->q_name, ((tb_o_Var *)tb_nn(v_b, 1262))->q_name) == 0) && ((tb_o_Var *)tb_nn(v_a, 1262))->q_is_shared) && (!((tb_o_Var *)tb_nn(v_b, 1262))->q_is_shared))) {
                                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1263))->q_line, "в by vec переменную, которую копишь через += или -=, нельзя читать внутри цикла", ((tb_o_Var *)tb_nn(v_a, 1263))->q_name);
                            }
                        }
                    }
                }
            }
        }
    } else {
        f_check__check_block(v_c, v_body);
    }
    { long long tb_v210 = 1LL; long long *tb_p210 = &(((tb_o_Comp *)tb_nn(v_c, 1266))->q_loop_depth); *tb_p210 -= tb_v210; }
}

static void f_check__check_block(tb_o_Comp * v_c, tb_o_Node * v_block) {
    f_check__open_scope(v_c);
    {
        tb_list * tb_l211 = ((tb_o_Node *)tb_nn(v_block, 1270))->q_kids;
        for (long long tb_i211 = 0; tb_i211 < tb_l211->len; tb_i211++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l211->data)[tb_i211];
            f_check__check_statement(v_c, v_s);
        }
    }
    f_check__close_scope(v_c);
}

static void f_check__check_func(tb_o_Comp * v_c, long long v_fi) {
    tb_o_Func * v_f = (*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1275))->q_funcs, v_fi, sizeof(tb_o_Func *), 1275));
    { long long tb_v212 = v_fi; long long *tb_p212 = &(((tb_o_Comp *)tb_nn(v_c, 1276))->q_current); *tb_p212 = tb_v212; }
    { const char * tb_v213 = ((tb_o_Func *)tb_nn(v_f, 1277))->q_mod; const char * *tb_p213 = &(((tb_o_Comp *)tb_nn(v_c, 1277))->q_cur_mod); *tb_p213 = tb_v213; }
    { const char * tb_v214 = ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1278))->q_node, 1278))->q_file; const char * *tb_p214 = &(((tb_o_Comp *)tb_nn(v_c, 1278))->q_file_name); *tb_p214 = tb_v214; }
    { long long tb_v215 = 1LL; long long *tb_p215 = &(((tb_o_Func *)tb_nn(v_f, 1279))->q_state); *tb_p215 = tb_v215; }
    { long long tb_v216 = 0LL; long long *tb_p216 = &(((tb_o_Comp *)tb_nn(v_c, 1280))->q_loop_depth); *tb_p216 = tb_v216; }
    f_check__open_scope(v_c);
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1282))->q_node, 1282))->q_kids, 0LL, sizeof(tb_o_Node *), 1282));
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1283))->q_node, 1283))->q_kids, 2LL, sizeof(tb_o_Node *), 1283));
    {
        tb_list * tb_l217 = ((tb_o_Node *)tb_nn(v_params, 1284))->q_kids;
        for (long long tb_i217 = 0; tb_i217 < tb_l217->len; tb_i217++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l217->data)[tb_i217];
            f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_p, 1285))->q_name, ((tb_o_Node *)tb_nn(v_p, 1285))->q_ty, 0, ((tb_o_Node *)tb_nn(v_p, 1285))->q_is_link, ((tb_o_Node *)tb_nn(v_p, 1285))->q_line);
        }
    }
    {
        tb_list * tb_l218 = ((tb_o_Node *)tb_nn(v_outputs, 1286))->q_kids;
        for (long long tb_i218 = 0; tb_i218 < tb_l218->len; tb_i218++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l218->data)[tb_i218];
            long long v_same_param = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_o, 1287))->q_name);
            if ((v_same_param >= 0LL)) {
                if ((((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1290))->q_vars, v_same_param, sizeof(tb_o_Var *), 1290)), 1290))->q_type != ((tb_o_Node *)tb_nn(v_o, 1290))->q_ty)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1291))->q_line, "выход и параметр с одним именем должны быть одного типа", ((tb_o_Node *)tb_nn(v_o, 1291))->q_name);
                }
                { _Bool tb_v219 = 1; _Bool *tb_p219 = &(((tb_o_Node *)tb_nn(v_o, 1293))->q_is_const); *tb_p219 = tb_v219; }
            } else {
                f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_o, 1295))->q_name, ((tb_o_Node *)tb_nn(v_o, 1295))->q_ty, 0, 0, ((tb_o_Node *)tb_nn(v_o, 1295))->q_line);
            }
        }
    }
    f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1296))->q_node, 1296))->q_kids, 1LL, sizeof(tb_o_Node *), 1296)));
    f_check__close_scope(v_c);
    if ((((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1298))->q_node, 1298))->q_ty < 0LL)) {
        { long long tb_v220 = 0LL; long long *tb_p220 = &(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1299))->q_node, 1299))->q_ty); *tb_p220 = tb_v220; }
    }
    { long long tb_v221 = 2LL; long long *tb_p221 = &(((tb_o_Func *)tb_nn(v_f, 1300))->q_state); *tb_p221 = tb_v221; }
}

static void f_check__register_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_o, 1303))->q_name) >= 0LL) || f_check__is_builtin(((tb_o_Node *)tb_nn(v_o, 1303))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1304))->q_line, "это имя уже занято", ((tb_o_Node *)tb_nn(v_o, 1304))->q_name);
    }
    if (((((tb_o_Comp *)tb_nn(v_c, 1305))->q_objs)->len == 500LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1306))->q_line, "слишком много obj", "");
    }
    { tb_o_Node * tb_v222 = v_o; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1307))->q_objs) = tb_v222; }
}

static void f_check__register_func(tb_o_Comp * v_c, tb_o_Node * v_fn) {
    if ((((f_check__find_func_in(v_c, ((tb_o_Node *)tb_nn(v_fn, 1310))->q_name, ((tb_o_Node *)tb_nn(v_fn, 1310))->q_mod) >= 0LL) || (f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_fn, 1310))->q_name) >= 0LL)) || f_check__is_builtin(((tb_o_Node *)tb_nn(v_fn, 1310))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_fn, 1311))->q_line, "это имя уже занято", ((tb_o_Node *)tb_nn(v_fn, 1311))->q_name);
    }
    if (((((tb_o_Comp *)tb_nn(v_c, 1312))->q_funcs)->len == 1000LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_fn, 1313))->q_line, "слишком много функций", "");
    }
    { tb_o_Func * tb_v223 = tb_new_Func(((tb_o_Node *)tb_nn(v_fn, 1314))->q_name, ((tb_o_Node *)tb_nn(v_fn, 1314))->q_mod, v_fn, 0LL); *(tb_o_Func * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1314))->q_funcs) = tb_v223; }
}

static _Bool f_check__has_fail(tb_o_Node * v_n) {
    _Bool v_r = 0;
    if (((strcmp(((tb_o_Node *)tb_nn(v_n, 1318))->q_kind, "fail") == 0) || (strcmp(((tb_o_Node *)tb_nn(v_n, 1318))->q_kind, "try") == 0))) {
        return 1;
    }
    {
        tb_list * tb_l224 = ((tb_o_Node *)tb_nn(v_n, 1320))->q_kids;
        for (long long tb_i224 = 0; tb_i224 < tb_l224->len; tb_i224++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l224->data)[tb_i224];
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
    if (((strcmp(((tb_o_Node *)tb_nn(v_n, 1327))->q_kind, "return") == 0) && ((((tb_o_Node *)tb_nn(v_n, 1327))->q_kids)->len > 0LL))) {
        return 1;
    }
    {
        tb_list * tb_l225 = ((tb_o_Node *)tb_nn(v_n, 1329))->q_kids;
        for (long long tb_i225 = 0; tb_i225 < tb_l225->len; tb_i225++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l225->data)[tb_i225];
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
        tb_list * tb_l226 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_fn, 1335))->q_kids, 0LL, sizeof(tb_o_Node *), 1335)), 1335))->q_kids;
        for (long long tb_i226 = 0; tb_i226 < tb_l226->len; tb_i226++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l226->data)[tb_i226];
            if ((((tb_o_Node *)tb_nn(v_p, 1336))->q_ty < 0LL)) {
                return 0;
            }
        }
    }
    return 1;
    tb_crash(1334, "функция links_known дошла до конца без return");
    return 0;
}

static void f_check__resolve_params(tb_o_Comp * v_c, tb_o_Node * v_params) {
    {
        long long tb_end227 = ((((tb_o_Node *)tb_nn(v_params, 1341))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end227; v_i++) {
            tb_o_Node * v_p = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1342))->q_kids, v_i, sizeof(tb_o_Node *), 1342));
            {
                long long tb_end228 = (v_i - 1LL);
                for (long long v_j = 0LL; v_j <= tb_end228; v_j++) {
                    if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1344))->q_kids, v_j, sizeof(tb_o_Node *), 1344)), 1344))->q_name, ((tb_o_Node *)tb_nn(v_p, 1344))->q_name) == 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_p, 1345))->q_line, "это имя уже есть в списке", ((tb_o_Node *)tb_nn(v_p, 1345))->q_name);
                    }
                }
            }
            if (((tb_o_Node *)tb_nn(v_p, 1346))->q_has_tsyn) {
                { long long tb_v229 = f_check__resolve_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1347))->q_tsyn); long long *tb_p229 = &(((tb_o_Node *)tb_nn(v_p, 1347))->q_ty); *tb_p229 = tb_v229; }
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
    { long long tb_v230 = (-1LL); *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1355))->q_temp_types) = tb_v230; }
    { long long tb_v231 = (-1LL); long long *tb_p231 = &(((tb_o_Comp *)tb_nn(v_c, 1356))->q_par_base); *tb_p231 = tb_v231; }
    { long long tb_v232 = (-1LL); long long *tb_p232 = &(((tb_o_Comp *)tb_nn(v_c, 1357))->q_temp_base); *tb_p232 = tb_v232; }
    const char * v_main_mod = "";
    if (((((tb_o_Node *)tb_nn(v_program, 1361))->q_kids)->len > 0LL)) {
        v_main_mod = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1362))->q_kids, 0LL, sizeof(tb_o_Node *), 1362)), 1362))->q_mod;
    }
    {
        tb_list * tb_l233 = ((tb_o_Node *)tb_nn(v_program, 1363))->q_kids;
        for (long long tb_i233 = 0; tb_i233 < tb_l233->len; tb_i233++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l233->data)[tb_i233];
            if ((!f_check__is_module(v_c, ((tb_o_Node *)tb_nn(v_top, 1364))->q_mod))) {
                { const char * tb_v234 = ((tb_o_Node *)tb_nn(v_top, 1365))->q_mod; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1365))->q_all_mods) = tb_v234; }
            }
            if ((strcmp(((tb_o_Node *)tb_nn(v_top, 1366))->q_kind, "use") == 0)) {
                if (((((tb_o_Comp *)tb_nn(v_c, 1367))->q_uses)->len == 500LL)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_top, 1368))->q_line, "слишком много use", "");
                }
                { tb_o_Use * tb_v235 = tb_new_Use(((tb_o_Node *)tb_nn(v_top, 1369))->q_mod, ((tb_o_Node *)tb_nn(v_top, 1369))->q_name); *(tb_o_Use * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1369))->q_uses) = tb_v235; }
            }
        }
    }
    {
        tb_list * tb_l236 = ((tb_o_Node *)tb_nn(v_program, 1371))->q_kids;
        for (long long tb_i236 = 0; tb_i236 < tb_l236->len; tb_i236++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l236->data)[tb_i236];
            { const char * tb_v237 = ((tb_o_Node *)tb_nn(v_top, 1372))->q_file; const char * *tb_p237 = &(((tb_o_Comp *)tb_nn(v_c, 1372))->q_file_name); *tb_p237 = tb_v237; }
            if ((strcmp(((tb_o_Node *)tb_nn(v_top, 1373))->q_kind, "obj") == 0)) {
                f_check__register_obj(v_c, v_top);
            } else if ((strcmp(((tb_o_Node *)tb_nn(v_top, 1375))->q_kind, "func") == 0)) {
                f_check__register_func(v_c, v_top);
                { _Bool tb_v238 = f_check__has_fail((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1377))->q_kids, 1LL, sizeof(tb_o_Node *), 1377))); _Bool *tb_p238 = &(((tb_o_Node *)tb_nn(v_top, 1377))->q_can_fail); *tb_p238 = tb_v238; }
                if (((strcmp(((tb_o_Node *)tb_nn(v_top, 1378))->q_name, "main") == 0) && (strcmp(((tb_o_Node *)tb_nn(v_top, 1378))->q_mod, v_main_mod) != 0))) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_top, 1379))->q_line, "main бывает только в главном файле, в модуле её быть не должно", "");
                }
            }
        }
    }
    {
        tb_list * tb_l239 = ((tb_o_Node *)tb_nn(v_program, 1381))->q_kids;
        for (long long tb_i239 = 0; tb_i239 < tb_l239->len; tb_i239++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l239->data)[tb_i239];
            { const char * tb_v240 = ((tb_o_Node *)tb_nn(v_top, 1382))->q_file; const char * *tb_p240 = &(((tb_o_Comp *)tb_nn(v_c, 1382))->q_file_name); *tb_p240 = tb_v240; }
            if ((strcmp(((tb_o_Node *)tb_nn(v_top, 1383))->q_kind, "obj") == 0)) {
                f_check__resolve_params(v_c, v_top);
            } else if ((strcmp(((tb_o_Node *)tb_nn(v_top, 1385))->q_kind, "func") == 0)) {
                f_check__resolve_params(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1386))->q_kids, 0LL, sizeof(tb_o_Node *), 1386)));
                f_check__resolve_params(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1387))->q_kids, 2LL, sizeof(tb_o_Node *), 1387)));
                tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1388))->q_kids, 2LL, sizeof(tb_o_Node *), 1388));
                if (((((tb_o_Node *)tb_nn(v_outputs, 1389))->q_kids)->len == 1LL)) {
                    { long long tb_v241 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1390))->q_kids, 0LL, sizeof(tb_o_Node *), 1390)), 1390))->q_ty; long long *tb_p241 = &(((tb_o_Node *)tb_nn(v_top, 1390))->q_ty); *tb_p241 = tb_v241; }
                } else if (((((tb_o_Node *)tb_nn(v_outputs, 1391))->q_kids)->len > 1LL)) {
                    { long long tb_v242 = f_check__multi_type(v_c, v_top); long long *tb_p242 = &(((tb_o_Node *)tb_nn(v_top, 1392))->q_ty); *tb_p242 = tb_v242; }
                } else if ((!f_check__has_value_return((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1393))->q_kids, 1LL, sizeof(tb_o_Node *), 1393))))) {
                    { long long tb_v243 = 0LL; long long *tb_p243 = &(((tb_o_Node *)tb_nn(v_top, 1395))->q_ty); *tb_p243 = tb_v243; }
                }
            }
        }
    }
    long long v_main_func = f_check__find_func_in(v_c, "main", v_main_mod);
    if (((((tb_o_Node *)tb_nn(v_program, 1397))->q_kids)->len > 0LL)) {
        { const char * tb_v244 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1398))->q_kids, 0LL, sizeof(tb_o_Node *), 1398)), 1398))->q_file; const char * *tb_p244 = &(((tb_o_Comp *)tb_nn(v_c, 1398))->q_file_name); *tb_p244 = tb_v244; }
    }
    if ((v_main_func < 0LL)) {
        f_errors__error_at(v_c, 0LL, "нет функции main: программа начинается с func main()", "");
    }
    tb_o_Node * v_main_node = ((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1401))->q_funcs, v_main_func, sizeof(tb_o_Func *), 1401)), 1401))->q_node;
    if ((((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_main_node, 1402))->q_kids, 0LL, sizeof(tb_o_Node *), 1402)), 1402))->q_kids)->len != 0LL) || ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_main_node, 1402))->q_kids, 2LL, sizeof(tb_o_Node *), 1402)), 1402))->q_kids)->len != 0LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_main_node, 1403))->q_line, "у main не бывает параметров и gives", "");
    }
    f_check__check_func(v_c, v_main_func);
    _Bool v_progress = 1;
    while (v_progress) {
        v_progress = 0;
        {
            long long tb_end245 = ((((tb_o_Comp *)tb_nn(v_c, 1410))->q_funcs)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end245; v_i++) {
                if (((((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1411))->q_funcs, v_i, sizeof(tb_o_Func *), 1411)), 1411))->q_state != 0LL) || (!f_check__links_known(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1411))->q_funcs, v_i, sizeof(tb_o_Func *), 1411)), 1411))->q_node)))) {
                    continue;
                }
                f_check__cut_vars(v_c, 0LL);
                { long long tb_v246 = 0LL; long long *tb_p246 = &(((tb_o_Comp *)tb_nn(v_c, 1414))->q_depth); *tb_p246 = tb_v246; }
                { long long tb_v247 = 0LL; long long *tb_p247 = &(((tb_o_Comp *)tb_nn(v_c, 1415))->q_var_base); *tb_p247 = tb_v247; }
                f_check__check_func(v_c, v_i);
                v_progress = 1;
            }
        }
    }
    {
        tb_list * tb_l248 = ((tb_o_Comp *)tb_nn(v_c, 1418))->q_funcs;
        for (long long tb_i248 = 0; tb_i248 < tb_l248->len; tb_i248++) {
            tb_o_Func * const v_f = ((tb_o_Func * *)tb_l248->data)[tb_i248];
            if ((((tb_o_Func *)tb_nn(v_f, 1419))->q_state == 0LL)) {
                { const char * tb_v249 = ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1420))->q_node, 1420))->q_file; const char * *tb_p249 = &(((tb_o_Comp *)tb_nn(v_c, 1420))->q_file_name); *tb_p249 = tb_v249; }
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1421))->q_node, 1421))->q_line, "функция с link ни разу не вызвана, поэтому тип ярлыка неизвестен", ((tb_o_Func *)tb_nn(v_f, 1421))->q_name);
            }
        }
    }
    f_check__check_piece_leaks(v_c);
}

static void f_emit__put(tb_o_Comp * v_c, const char * v_s) {
    { const char * tb_v250 = v_s; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 10))->q_out) = tb_v250; }
}

static const char * f_emit__fname(tb_o_Node * v_f) {
    const char * v_mod = "";
    {
        const char * tb_l251 = ((tb_o_Node *)tb_nn(v_f, 15))->q_mod;
        for (long long tb_i251 = 0; tb_l251[tb_i251] != 0; tb_i251++) {
            const char * const v_ch = tb_chr((unsigned char)tb_l251[tb_i251]);
            long long v_k = tb_code(v_ch, 16);
            if (((((v_k >= 97LL) && (v_k <= 122LL)) || ((v_k >= 65LL) && (v_k <= 90LL))) || ((v_k >= 48LL) && (v_k <= 57LL)))) {
                v_mod = tb_cat(v_mod, v_ch);
            } else {
                v_mod = tb_cat(v_mod, "_");
            }
        }
    }
    return tb_cat(tb_cat(tb_cat("f_", v_mod), "__"), ((tb_o_Node *)tb_nn(v_f, 21))->q_name);
    tb_crash(13, "функция fname дошла до конца без return");
    return "";
}

static const char * f_emit__c_type(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    if ((strcmp(v_k, "int") == 0)) {
        return "long long";
    }
    if ((strcmp(v_k, "dec") == 0)) {
        return "double";
    }
    if ((strcmp(v_k, "bool") == 0)) {
        return "_Bool";
    }
    if ((strcmp(v_k, "text") == 0)) {
        return "const char *";
    }
    if ((strcmp(v_k, "list") == 0)) {
        if (f_check__is_cols_list(v_c, v_t)) {
            return tb_cat(tb_cat("tb_cols_", ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 35))->q_types, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 35))->q_types, v_t, sizeof(tb_o_Type *), 35)), 35))->q_elem, sizeof(tb_o_Type *), 35)), 35))->q_obj_node, 35))->q_name), " *");
        }
        return "tb_list *";
    }
    if ((strcmp(v_k, "dict") == 0)) {
        return "tb_dict *";
    }
    if ((strcmp(v_k, "obj") == 0)) {
        if (f_check__is_cols_obj(v_c, v_t)) {
            return tb_cat("tb_row_", ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 41))->q_types, v_t, sizeof(tb_o_Type *), 41)), 41))->q_obj_node, 41))->q_name);
        }
        return tb_cat(tb_cat("tb_o_", ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 42))->q_types, v_t, sizeof(tb_o_Type *), 42)), 42))->q_obj_node, 42))->q_name), " *");
    }
    if ((strcmp(v_k, "multi") == 0)) {
        return tb_cat("tb_r_", f_emit__fname(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 44))->q_types, v_t, sizeof(tb_o_Type *), 44)), 44))->q_func_node));
    }
    if ((strcmp(v_k, "task") == 0)) {
        return "tb_task *";
    }
    return "void";
    tb_crash(23, "функция c_type дошла до конца без return");
    return "";
}

static void f_emit__indent(tb_o_Comp * v_c, long long v_depth) {
    {
        long long tb_end252 = v_depth;
        for (long long v_i = 1LL; v_i <= tb_end252; v_i++) {
            f_emit__put(v_c, "    ");
        }
    }
}

static void f_emit__emit_default(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    if ((strcmp(v_k, "int") == 0)) {
        f_emit__put(v_c, "0LL");
    } else if ((strcmp(v_k, "dec") == 0)) {
        f_emit__put(v_c, "0.0");
    } else if ((strcmp(v_k, "text") == 0)) {
        f_emit__put(v_c, "\"\"");
    } else if (((strcmp(v_k, "list") == 0) && f_check__is_cols_list(v_c, v_t))) {
        f_emit__put(v_c, tb_cat(tb_cat("tb_cols_new_", f_emit__cols_name(v_c, v_t)), "()"));
    } else if (((strcmp(v_k, "obj") == 0) && f_check__is_cols_obj(v_c, v_t))) {
        f_emit__put(v_c, tb_cat(tb_cat("(", f_emit__c_type(v_c, v_t)), "){0, 0}"));
    } else if ((strcmp(v_k, "list") == 0)) {
        f_emit__put(v_c, tb_cat(tb_cat("tb_list_new(sizeof(", f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 66))->q_types, v_t, sizeof(tb_o_Type *), 66)), 66))->q_elem)), "))"));
    } else if ((strcmp(v_k, "dict") == 0)) {
        const char * v_is_text = "0";
        if ((strcmp(f_check__kind_of(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 69))->q_types, v_t, sizeof(tb_o_Type *), 69)), 69))->q_key), "text") == 0)) {
            v_is_text = "1";
        }
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("tb_dict_new(", v_is_text), ", sizeof("), f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 71))->q_types, v_t, sizeof(tb_o_Type *), 71)), 71))->q_elem)), "))"));
    } else {
        f_emit__put(v_c, "0");
    }
}

static const char * f_emit__cols_name(tb_o_Comp * v_c, long long v_t) {
    if ((strcmp(f_check__kind_of(v_c, v_t), "list") == 0)) {
        return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 78))->q_types, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 78))->q_types, v_t, sizeof(tb_o_Type *), 78)), 78))->q_elem, sizeof(tb_o_Type *), 78)), 78))->q_obj_node, 78))->q_name;
    }
    return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 79))->q_types, v_t, sizeof(tb_o_Type *), 79)), 79))->q_obj_node, 79))->q_name;
    tb_crash(76, "функция cols_name дошла до конца без return");
    return "";
}

static _Bool f_emit__is_simple(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    return ((((strcmp(v_k, "int") == 0) || (strcmp(v_k, "dec") == 0)) || (strcmp(v_k, "bool") == 0)) || (strcmp(v_k, "text") == 0));
    tb_crash(81, "функция is_simple дошла до конца без return");
    return 0;
}

static void f_emit__emit_key(tb_o_Comp * v_c, long long v_dt, tb_o_Node * v_key) {
    f_emit__put(v_c, tb_cat(tb_cat("&(", f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 87))->q_types, v_dt, sizeof(tb_o_Type *), 87)), 87))->q_key)), "){"));
    f_emit__emit_expr(v_c, v_key);
    f_emit__put(v_c, "}");
}

static void f_emit__emit_as(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_want) {
    if (((strcmp(f_check__kind_of(v_c, v_want), "dec") == 0) && (strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 95))->q_ty), "int") == 0))) {
        f_emit__put(v_c, "(double)(");
        f_emit__emit_expr(v_c, v_n);
        f_emit__put(v_c, ")");
        return;
    }
    f_emit__emit_expr(v_c, v_n);
}

static void f_emit__emit_as_text(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 104))->q_ty);
    if ((strcmp(v_k, "text") == 0)) {
        f_emit__emit_expr(v_c, v_n);
        return;
    }
    if ((strcmp(v_k, "int") == 0)) {
        f_emit__put(v_c, "tb_itos(");
    } else if ((strcmp(v_k, "dec") == 0)) {
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
        long long tb_end253 = ((((tb_o_Node *)tb_nn(v_n, 119))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end253; v_i++) {
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 120))->q_kids, v_i, sizeof(tb_o_Node *), 120)), 120))->q_kids, 0LL, sizeof(tb_o_Node *), 120));
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", tb_ps(\" \"), ");
            }
            f_emit__put(v_c, tb_cat(tb_cat("tb_show_", tb_itos(((tb_o_Node *)tb_nn(v_value, 123))->q_ty)), "("));
            f_emit__emit_expr(v_c, v_value);
            f_emit__put(v_c, ")");
        }
    }
    if (((((tb_o_Node *)tb_nn(v_n, 126))->q_kids)->len > 0LL)) {
        f_emit__put(v_c, ", ");
    }
    f_emit__put(v_c, "tb_ps(\"\\n\"))");
}

static void f_emit__emit_args(tb_o_Comp * v_c, tb_o_Node * v_n, _Bool v_with_line) {
    {
        long long tb_end254 = ((((tb_o_Node *)tb_nn(v_n, 132))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end254; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 135))->q_kids, v_i, sizeof(tb_o_Node *), 135)), 135))->q_kids, 0LL, sizeof(tb_o_Node *), 135)));
        }
    }
    if (v_with_line) {
        if (((((tb_o_Node *)tb_nn(v_n, 137))->q_kids)->len > 0LL)) {
            f_emit__put(v_c, ", ");
        }
        f_emit__put(v_c, tb_cat("", tb_itos(((tb_o_Node *)tb_nn(v_n, 139))->q_line)));
    }
    f_emit__put(v_c, ")");
}

static void f_emit__emit_builtin(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_name = ((tb_o_Node *)tb_nn(v_n, 143))->q_name;
    if ((strcmp(v_name, "print") == 0)) {
        f_emit__emit_print(v_c, v_n);
        return;
    }
    if ((strcmp(v_name, "args") == 0)) {
        f_emit__put(v_c, "tb_args");
        return;
    }
    if ((strcmp(v_name, "input") == 0)) {
        f_emit__put(v_c, "tb_input()");
        return;
    }
    tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 153))->q_kids, 0LL, sizeof(tb_o_Node *), 153)), 153))->q_kids, 0LL, sizeof(tb_o_Node *), 153));
    if ((strcmp(v_name, "len") == 0)) {
        const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 155))->q_ty);
        if ((strcmp(v_k, "text") == 0)) {
            f_emit__put(v_c, "(long long)strlen(");
            f_emit__emit_expr(v_c, v_a);
            f_emit__put(v_c, ")");
        } else if ((strcmp(v_k, "dict") == 0)) {
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
    if ((strcmp(v_name, "sort") == 0)) {
        const char * v_k = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 170))->q_types, ((tb_o_Node *)tb_nn(v_a, 170))->q_ty, sizeof(tb_o_Type *), 170)), 170))->q_elem);
        f_emit__put(v_c, "tb_sort_list(");
        f_emit__emit_expr(v_c, v_a);
        if ((strcmp(v_k, "int") == 0)) {
            f_emit__put(v_c, ", tb_cmp_int)");
        } else if ((strcmp(v_k, "dec") == 0)) {
            f_emit__put(v_c, ", tb_cmp_dec)");
        } else {
            f_emit__put(v_c, ", tb_cmp_text)");
        }
        return;
    }
    if ((strcmp(v_name, "abs") == 0)) {
        if ((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 181))->q_ty), "int") == 0)) {
            f_emit__put(v_c, "tb_absi(");
        } else {
            f_emit__put(v_c, "fabs(");
        }
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ")");
        return;
    }
    if ((strcmp(v_name, "sqrt") == 0)) {
        f_emit__put(v_c, "tb_sqrt((double)(");
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, tb_cat(tb_cat("), ", tb_itos(((tb_o_Node *)tb_nn(v_n, 191))->q_line)), ")"));
        return;
    }
    if (((strcmp(v_name, "min") == 0) || (strcmp(v_name, "max") == 0))) {
        _Bool v_ints = (strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 194))->q_ty), "int") == 0);
        if (v_ints) {
            f_emit__put(v_c, tb_cat(tb_cat("tb_", v_name), "i("));
            f_emit__emit_expr(v_c, v_a);
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 199))->q_kids, 1LL, sizeof(tb_o_Node *), 199)), 199))->q_kids, 0LL, sizeof(tb_o_Node *), 199)));
        } else {
            f_emit__put(v_c, tb_cat(tb_cat("tb_", v_name), "d("));
            f_emit__emit_as(v_c, v_a, ((tb_o_Node *)tb_nn(v_n, 202))->q_ty);
            f_emit__put(v_c, ", ");
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 204))->q_kids, 1LL, sizeof(tb_o_Node *), 204)), 204))->q_kids, 0LL, sizeof(tb_o_Node *), 204)), ((tb_o_Node *)tb_nn(v_n, 204))->q_ty);
        }
        f_emit__put(v_c, ")");
        return;
    }
    tb_list * v_c_names = tb_list_from(60, sizeof(const char *), (const char *[]){"round", "tb_round", "0", "char", "tb_chr", "0", "cut", "tb_cut", "1", "code", "tb_code", "1", "to_int", "tb_to_int", "0", "to_dec", "tb_to_dec", "0", "read_file", "tb_read_file", "0", "write_file", "tb_write_file", "0", "split", "tb_split", "1", "join", "tb_join", "0", "find", "tb_find", "0", "replace", "tb_replace", "1", "upper", "tb_upper", "0", "lower", "tb_lower", "0", "trim", "tb_trim", "0", "starts_with", "tb_starts", "0", "ends_with", "tb_ends", "0", "run", "tb_run", "0", "exit", "tb_exit", "0", "file_exists", "tb_file_exists", "0"});
    long long v_i = 0LL;
    while ((v_i < (v_c_names)->len)) {
        if ((strcmp((*(const char * *)tb_at(v_c_names, v_i, sizeof(const char *), 211)), v_name) == 0)) {
            f_emit__put(v_c, tb_cat((*(const char * *)tb_at(v_c_names, (v_i + 1LL), sizeof(const char *), 212)), "("));
            f_emit__emit_args(v_c, v_n, (strcmp((*(const char * *)tb_at(v_c_names, (v_i + 2LL), sizeof(const char *), 213)), "1") == 0));
            return;
        }
        { long long tb_v255 = 3LL; long long *tb_p255 = &(v_i); *tb_p255 += tb_v255; }
    }
    f_emit__put(v_c, tb_cat(tb_cat("/* неизвестная встроенная ", v_name), " */0"));
}

static void f_emit__emit_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if ((!((tb_o_Node *)tb_nn(v_n, 219))->q_has_ref)) {
        f_emit__emit_builtin(v_c, v_n);
        return;
    }
    if ((strcmp(((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 222))->q_ref, 222))->q_kind, "obj") == 0)) {
        tb_o_Node * v_o = ((tb_o_Node *)tb_nn(v_n, 223))->q_ref;
        f_emit__put(v_c, tb_cat(tb_cat("tb_new_", ((tb_o_Node *)tb_nn(v_o, 224))->q_name), "("));
        {
            long long tb_end256 = ((((tb_o_Node *)tb_nn(v_o, 225))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end256; v_i++) {
                if ((v_i > 0LL)) {
                    f_emit__put(v_c, ", ");
                }
                if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 228))->q_kids, v_i, sizeof(tb_o_Node *), 228)), 228))->q_kind, "missing") == 0)) {
                    f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 229))->q_kids, v_i, sizeof(tb_o_Node *), 229)), 229))->q_ty);
                } else {
                    f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 231))->q_kids, v_i, sizeof(tb_o_Node *), 231)), 231))->q_kids, 0LL, sizeof(tb_o_Node *), 231)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 231))->q_kids, v_i, sizeof(tb_o_Node *), 231)), 231))->q_ty);
                }
            }
        }
        f_emit__put(v_c, ")");
        return;
    }
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 234))->q_ref, 234))->q_kids, 0LL, sizeof(tb_o_Node *), 234));
    f_emit__put(v_c, tb_cat(f_emit__fname(((tb_o_Node *)tb_nn(v_n, 235))->q_ref), "("));
    {
        long long tb_end257 = ((((tb_o_Node *)tb_nn(v_n, 236))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end257; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 239))->q_kids, v_i, sizeof(tb_o_Node *), 239)), 239))->q_is_link) {
                f_emit__put(v_c, "&(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 241))->q_kids, v_i, sizeof(tb_o_Node *), 241)), 241))->q_kids, 0LL, sizeof(tb_o_Node *), 241)));
                f_emit__put(v_c, ")");
            } else {
                f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 244))->q_kids, v_i, sizeof(tb_o_Node *), 244)), 244))->q_kids, 0LL, sizeof(tb_o_Node *), 244)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 244))->q_kids, v_i, sizeof(tb_o_Node *), 244)), 244))->q_ty);
            }
        }
    }
    f_emit__put(v_c, ")");
}

static void f_emit__emit_dict(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Type * v_t = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 248))->q_types, ((tb_o_Node *)tb_nn(v_n, 248))->q_ty, sizeof(tb_o_Type *), 248));
    const char * v_kt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 249))->q_key);
    const char * v_vt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 250))->q_elem);
    const char * v_is_text = "0";
    if ((strcmp(f_check__kind_of(v_c, ((tb_o_Type *)tb_nn(v_t, 252))->q_key), "text") == 0)) {
        v_is_text = "1";
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("tb_dict_from(", v_is_text), ", sizeof("), v_vt), "), "), tb_itos(tb_div((((tb_o_Node *)tb_nn(v_n, 254))->q_kids)->len, 2LL, 254))), ", ("), v_kt), "[]){"));
    long long v_i = 0LL;
    while ((v_i < (((tb_o_Node *)tb_nn(v_n, 256))->q_kids)->len)) {
        if ((v_i > 0LL)) {
            f_emit__put(v_c, ", ");
        }
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 259))->q_kids, v_i, sizeof(tb_o_Node *), 259)));
        { long long tb_v258 = 2LL; long long *tb_p258 = &(v_i); *tb_p258 += tb_v258; }
    }
    f_emit__put(v_c, tb_cat(tb_cat("}, (", v_vt), "[]){"));
    v_i = 1LL;
    while ((v_i < (((tb_o_Node *)tb_nn(v_n, 263))->q_kids)->len)) {
        if ((v_i > 1LL)) {
            f_emit__put(v_c, ", ");
        }
        f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 266))->q_kids, v_i, sizeof(tb_o_Node *), 266)), ((tb_o_Type *)tb_nn(v_t, 266))->q_elem);
        { long long tb_v259 = 2LL; long long *tb_p259 = &(v_i); *tb_p259 += tb_v259; }
    }
    f_emit__put(v_c, "})");
}

static void f_emit__emit_method(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_base = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 272))->q_kids, 0LL, sizeof(tb_o_Node *), 272));
    if ((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_base, 273))->q_ty), "dict") == 0)) {
        if (((strcmp(((tb_o_Node *)tb_nn(v_n, 274))->q_name, "values") == 0) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_n, 274))->q_ty))) {
            f_emit__put(v_c, tb_cat(tb_cat("tb_colsfrom_", f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn(v_n, 275))->q_ty)), "(("));
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, tb_cat(tb_cat(")->vals->len, (", f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 277))->q_types, ((tb_o_Node *)tb_nn(v_n, 277))->q_ty, sizeof(tb_o_Type *), 277)), 277))->q_elem)), " *)("));
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, ")->vals->data)");
            return;
        }
        if (((strcmp(((tb_o_Node *)tb_nn(v_n, 281))->q_name, "keys") == 0) || (strcmp(((tb_o_Node *)tb_nn(v_n, 281))->q_name, "values") == 0))) {
            f_emit__put(v_c, "tb_copy((");
            f_emit__emit_expr(v_c, v_base);
            if ((strcmp(((tb_o_Node *)tb_nn(v_n, 284))->q_name, "keys") == 0)) {
                f_emit__put(v_c, ")->keys)");
            } else {
                f_emit__put(v_c, ")->vals)");
            }
            return;
        }
        if ((strcmp(((tb_o_Node *)tb_nn(v_n, 289))->q_name, "has") == 0)) {
            f_emit__put(v_c, "tb_dhas(");
        } else {
            f_emit__put(v_c, "tb_dremove(");
        }
        f_emit__emit_expr(v_c, v_base);
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, ((tb_o_Node *)tb_nn(v_base, 295))->q_ty, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 295))->q_kids, 1LL, sizeof(tb_o_Node *), 295)), 295))->q_kids, 0LL, sizeof(tb_o_Node *), 295)));
        f_emit__put(v_c, ")");
        return;
    }
    f_emit__put(v_c, tb_cat(tb_cat("(*(", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 298))->q_ty)), " *)tb_pop("));
    f_emit__emit_expr(v_c, v_base);
    f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 300))->q_line)), "))"));
}

static void f_emit__emit_list(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 303))->q_types, ((tb_o_Node *)tb_nn(v_n, 303))->q_ty, sizeof(tb_o_Type *), 303)), 303))->q_elem;
    const char * v_elem = f_emit__c_type(v_c, v_elem_ty);
    if (f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_n, 305))->q_ty)) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("tb_colsfrom_", f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn(v_n, 306))->q_ty)), "("), tb_itos((((tb_o_Node *)tb_nn(v_n, 306))->q_kids)->len)), ", ("), v_elem), "[]){"));
    } else {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("tb_list_from(", tb_itos((((tb_o_Node *)tb_nn(v_n, 308))->q_kids)->len)), ", sizeof("), v_elem), "), ("), v_elem), "[]){"));
    }
    {
        long long tb_end260 = ((((tb_o_Node *)tb_nn(v_n, 309))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end260; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 312))->q_kids, v_i, sizeof(tb_o_Node *), 312)), v_elem_ty);
        }
    }
    f_emit__put(v_c, "})");
}

static void f_emit__emit_binop(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 316))->q_value;
    tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 317))->q_kids, 0LL, sizeof(tb_o_Node *), 317));
    tb_o_Node * v_b = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 318))->q_kids, 1LL, sizeof(tb_o_Node *), 318));
    if ((strcmp(v_op, "orelse") == 0)) {
        f_emit__put(v_c, tb_cat(tb_cat("(tb_t", tb_itos(((tb_o_Node *)tb_nn(v_n, 321))->q_temp_no)), " = "));
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ", tb_err ? (tb_err = 0, ");
        f_emit__emit_as(v_c, v_b, ((tb_o_Node *)tb_nn(v_n, 324))->q_ty);
        f_emit__put(v_c, tb_cat(tb_cat(") : tb_t", tb_itos(((tb_o_Node *)tb_nn(v_n, 325))->q_temp_no)), ")"));
        return;
    }
    if ((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 327))->q_ty), "text") == 0)) {
        f_emit__put(v_c, "tb_cat(");
        f_emit__emit_as_text(v_c, v_a);
        f_emit__put(v_c, ", ");
        f_emit__emit_as_text(v_c, v_b);
        f_emit__put(v_c, ")");
        return;
    }
    if ((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 334))->q_ty), "text") == 0)) {
        f_emit__put(v_c, "(strcmp(");
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, v_b);
        f_emit__put(v_c, tb_cat(tb_cat(") ", v_op), " 0)"));
        return;
    }
    if (((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 341))->q_ty), "int") == 0) && ((strcmp(v_op, "/") == 0) || (strcmp(v_op, "%") == 0)))) {
        if ((strcmp(v_op, "/") == 0)) {
            f_emit__put(v_c, "tb_div(");
        } else {
            f_emit__put(v_c, "tb_mod(");
        }
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, v_b);
        f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 349))->q_line)), ")"));
        return;
    }
    const char * v_c_op = v_op;
    if ((strcmp(v_op, "and") == 0)) {
        v_c_op = "&&";
    } else if ((strcmp(v_op, "or") == 0)) {
        v_c_op = "||";
    }
    f_emit__put(v_c, "(");
    if (((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 357))->q_ty), "dec") == 0) && (strcmp(v_op, "/") == 0))) {
        f_emit__emit_as(v_c, v_a, ((tb_o_Node *)tb_nn(v_n, 358))->q_ty);
    } else {
        f_emit__emit_expr(v_c, v_a);
    }
    f_emit__put(v_c, tb_cat(tb_cat(" ", v_c_op), " "));
    f_emit__emit_expr(v_c, v_b);
    f_emit__put(v_c, ")");
}

static void f_emit__emit_expr(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 366))->q_kind;
    if ((strcmp(v_k, "number") == 0)) {
        f_emit__put(v_c, tb_cat(((tb_o_Node *)tb_nn(v_n, 368))->q_value, "LL"));
    } else if (((strcmp(v_k, "decimal") == 0) || (strcmp(v_k, "string") == 0))) {
        f_emit__put(v_c, ((tb_o_Node *)tb_nn(v_n, 370))->q_value);
    } else if ((strcmp(v_k, "bool") == 0)) {
        if ((strcmp(((tb_o_Node *)tb_nn(v_n, 372))->q_value, "true") == 0)) {
            f_emit__put(v_c, "1");
        } else {
            f_emit__put(v_c, "0");
        }
    } else if ((((strcmp(v_k, "name") == 0) && ((((tb_o_Comp *)tb_nn(v_c, 376))->q_vec_names)->len > 0LL)) && (f_emit__vec_index(v_c, ((tb_o_Node *)tb_nn(v_n, 376))->q_name) >= 0LL))) {
        f_emit__put(v_c, (*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 377))->q_vec_repl, f_emit__vec_index(v_c, ((tb_o_Node *)tb_nn(v_n, 377))->q_name), sizeof(const char *), 377)));
    } else if ((strcmp(v_k, "name") == 0)) {
        if (((tb_o_Node *)tb_nn(v_n, 379))->q_is_link) {
            f_emit__put(v_c, tb_cat(tb_cat("(*v_", ((tb_o_Node *)tb_nn(v_n, 380))->q_name), ")"));
        } else {
            f_emit__put(v_c, tb_cat("v_", ((tb_o_Node *)tb_nn(v_n, 382))->q_name));
        }
    } else if ((strcmp(v_k, "list") == 0)) {
        f_emit__emit_list(v_c, v_n);
    } else if ((strcmp(v_k, "dict") == 0)) {
        f_emit__emit_dict(v_c, v_n);
    } else if (((strcmp(v_k, "try") == 0) || (strcmp(v_k, "else_err") == 0))) {
        f_emit__put(v_c, tb_cat("tb_v", tb_itos(((tb_o_Node *)tb_nn(v_n, 389))->q_temp_no)));
    } else if ((strcmp(v_k, "call") == 0)) {
        f_emit__emit_call(v_c, v_n);
    } else if ((strcmp(v_k, "method") == 0)) {
        f_emit__emit_method(v_c, v_n);
    } else if ((strcmp(v_k, "binop") == 0)) {
        f_emit__emit_binop(v_c, v_n);
    } else if ((strcmp(v_k, "unary") == 0)) {
        if ((strcmp(((tb_o_Node *)tb_nn(v_n, 397))->q_value, "not") == 0)) {
            f_emit__put(v_c, "(!");
        } else {
            f_emit__put(v_c, "(-");
        }
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 401))->q_kids, 0LL, sizeof(tb_o_Node *), 401)));
        f_emit__put(v_c, ")");
    } else if (((strcmp(v_k, "index") == 0) && (strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 403))->q_kids, 0LL, sizeof(tb_o_Node *), 403)), 403))->q_ty), "dict") == 0))) {
        f_emit__put(v_c, tb_cat(tb_cat("(*(", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 404))->q_ty)), " *)tb_dget("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 405))->q_kids, 0LL, sizeof(tb_o_Node *), 405)));
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 407))->q_kids, 0LL, sizeof(tb_o_Node *), 407)), 407))->q_ty, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 407))->q_kids, 1LL, sizeof(tb_o_Node *), 407)));
        f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 408))->q_line)), "))"));
    } else if (((strcmp(v_k, "index") == 0) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 409))->q_kids, 0LL, sizeof(tb_o_Node *), 409)), 409))->q_ty))) {
        f_emit__put(v_c, tb_cat(tb_cat("tb_colat_", f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 410))->q_kids, 0LL, sizeof(tb_o_Node *), 410)), 410))->q_ty)), "("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 411))->q_kids, 0LL, sizeof(tb_o_Node *), 411)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 413))->q_kids, 1LL, sizeof(tb_o_Node *), 413)));
        f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 414))->q_line)), ")"));
    } else if ((strcmp(v_k, "index") == 0)) {
        if ((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 416))->q_kids, 0LL, sizeof(tb_o_Node *), 416)), 416))->q_ty), "text") == 0)) {
            f_emit__put(v_c, "tb_char_at(");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 418))->q_kids, 0LL, sizeof(tb_o_Node *), 418)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 420))->q_kids, 1LL, sizeof(tb_o_Node *), 420)));
            f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 421))->q_line)), ")"));
        } else {
            const char * v_t = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 423))->q_ty);
            f_emit__put(v_c, tb_cat(tb_cat("(*(", v_t), " *)tb_at("));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 425))->q_kids, 0LL, sizeof(tb_o_Node *), 425)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 427))->q_kids, 1LL, sizeof(tb_o_Node *), 427)));
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(", sizeof(", v_t), "), "), tb_itos(((tb_o_Node *)tb_nn(v_n, 428))->q_line)), "))"));
        }
    } else if (((strcmp(v_k, "field") == 0) && (strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 429))->q_kids, 0LL, sizeof(tb_o_Node *), 429)), 429))->q_ty), "task") == 0))) {
        long long v_rt = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 430))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 430))->q_kids, 0LL, sizeof(tb_o_Node *), 430)), 430))->q_ty, sizeof(tb_o_Type *), 430)), 430))->q_elem;
        if ((v_rt == 0LL)) {
            f_emit__put(v_c, "((void)tb_wait(");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 433))->q_kids, 0LL, sizeof(tb_o_Node *), 433)));
            f_emit__put(v_c, "))");
        } else {
            f_emit__put(v_c, tb_cat(tb_cat("(*(", f_emit__c_type(v_c, v_rt)), " *)tb_wait("));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 437))->q_kids, 0LL, sizeof(tb_o_Node *), 437)));
            f_emit__put(v_c, "))");
        }
    } else if ((strcmp(v_k, "background") == 0)) {
        f_emit__emit_background(v_c, v_n);
    } else if (((strcmp(v_k, "field") == 0) && f_check__is_cols_obj(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 441))->q_kids, 0LL, sizeof(tb_o_Node *), 441)), 441))->q_ty))) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("(*tb_colf_", f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 443))->q_kids, 0LL, sizeof(tb_o_Node *), 443)), 443))->q_ty)), "_"), ((tb_o_Node *)tb_nn(v_n, 443))->q_name), "("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 444))->q_kids, 0LL, sizeof(tb_o_Node *), 444)));
        f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 445))->q_line)), "))"));
    } else if ((strcmp(v_k, "field") == 0)) {
        f_emit__put(v_c, tb_cat(tb_cat("((", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 447))->q_kids, 0LL, sizeof(tb_o_Node *), 447)), 447))->q_ty)), ")tb_nn("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 448))->q_kids, 0LL, sizeof(tb_o_Node *), 448)));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_n, 449))->q_line)), "))->q_"), ((tb_o_Node *)tb_nn(v_n, 449))->q_name));
    }
}

static void f_emit__emit_bail(tb_o_Comp * v_c) {
    tb_o_Node * v_f = ((tb_o_Comp *)tb_nn(v_c, 455))->q_cur_func;
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 456))->q_kids, 2LL, sizeof(tb_o_Node *), 456));
    if ((strcmp(((tb_o_Node *)tb_nn(v_f, 457))->q_name, "main") == 0)) {
        f_emit__put(v_c, "tb_die(tb_err);");
    } else if (((((tb_o_Node *)tb_nn(v_outputs, 459))->q_kids)->len == 1LL)) {
        f_emit__put(v_c, tb_cat(tb_cat("return v_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 460))->q_kids, 0LL, sizeof(tb_o_Node *), 460)), 460))->q_name), ";"));
    } else if (((((tb_o_Node *)tb_nn(v_outputs, 461))->q_kids)->len > 1LL)) {
        f_emit__put(v_c, tb_cat(tb_cat("return (", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 462))->q_ty)), "){"));
        {
            long long tb_end261 = ((((tb_o_Node *)tb_nn(v_outputs, 463))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end261; v_i++) {
                if ((v_i > 0LL)) {
                    f_emit__put(v_c, ", ");
                }
                f_emit__put(v_c, tb_cat("v_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 466))->q_kids, v_i, sizeof(tb_o_Node *), 466)), 466))->q_name));
            }
        }
        f_emit__put(v_c, "};");
    } else if ((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 468))->q_ty), "none") == 0)) {
        f_emit__put(v_c, "return;");
    } else {
        f_emit__put(v_c, "return ");
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_f, 472))->q_ty);
        f_emit__put(v_c, ";");
    }
}

static void f_emit__emit_pre(tb_o_Comp * v_c, tb_o_Node * v_v, long long v_depth) {
    if (((strcmp(((tb_o_Node *)tb_nn(v_v, 477))->q_kind, "try") != 0) && (strcmp(((tb_o_Node *)tb_nn(v_v, 477))->q_kind, "else_err") != 0))) {
        return;
    }
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 479))->q_kids, 0LL, sizeof(tb_o_Node *), 479));
    { long long tb_v262 = 1LL; long long *tb_p262 = &(((tb_o_Comp *)tb_nn(v_c, 480))->q_label); *tb_p262 += tb_v262; }
    { long long tb_v263 = ((tb_o_Comp *)tb_nn(v_c, 481))->q_label; long long *tb_p263 = &(((tb_o_Node *)tb_nn(v_v, 481))->q_temp_no); *tb_p263 = tb_v263; }
    f_emit__indent(v_c, v_depth);
    if ((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_call, 483))->q_ty), "none") != 0)) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_call, 484))->q_ty), " tb_v"), tb_itos(((tb_o_Comp *)tb_nn(v_c, 484))->q_label)), " = "));
    }
    f_emit__emit_expr(v_c, v_call);
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, v_depth);
    if ((strcmp(((tb_o_Node *)tb_nn(v_v, 488))->q_kind, "try") == 0)) {
        f_emit__put(v_c, "if (TB_RARE(tb_err != 0)) { ");
        f_emit__emit_bail(v_c);
        f_emit__put(v_c, " }\n");
        return;
    }
    f_emit__put(v_c, "if (tb_err != 0) {\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat("const char * const v_", ((tb_o_Node *)tb_nn(v_v, 495))->q_name), " = tb_err;\n"));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "tb_err = 0;\n");
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 498))->q_kids, 1LL, sizeof(tb_o_Node *), 498)), (v_depth + 1LL));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_target(tb_o_Comp * v_c, tb_o_Node * v_t) {
    if (((strcmp(((tb_o_Node *)tb_nn(v_t, 504))->q_kind, "index") == 0) && (strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 504))->q_kids, 0LL, sizeof(tb_o_Node *), 504)), 504))->q_ty), "dict") == 0))) {
        long long v_d = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 505))->q_kids, 0LL, sizeof(tb_o_Node *), 505)), 505))->q_ty;
        long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 506))->q_types, v_d, sizeof(tb_o_Type *), 506)), 506))->q_elem;
        const char * v_vt = f_emit__c_type(v_c, v_elem);
        f_emit__put(v_c, tb_cat(tb_cat("(*(", v_vt), " *)tb_dput("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 509))->q_kids, 0LL, sizeof(tb_o_Node *), 509)));
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, v_d, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 511))->q_kids, 1LL, sizeof(tb_o_Node *), 511)));
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
    if (((((tb_o_Node *)tb_nn(v_n, 522))->q_kids)->len == 1LL)) {
        f_emit__emit_pre(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 523))->q_kids, 0LL, sizeof(tb_o_Node *), 523)), v_depth);
    }
    f_emit__indent(v_c, v_depth);
    if (((tb_o_Node *)tb_nn(v_n, 525))->q_creates) {
        f_emit__put(v_c, tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 526))->q_ty), " "));
        if (((tb_o_Node *)tb_nn(v_n, 527))->q_is_const) {
            f_emit__put(v_c, "const ");
        }
        f_emit__put(v_c, tb_cat(tb_cat("v_", ((tb_o_Node *)tb_nn(v_n, 529))->q_name), " = "));
    } else if (((tb_o_Node *)tb_nn(v_n, 530))->q_is_link) {
        f_emit__put(v_c, tb_cat(tb_cat("(*v_", ((tb_o_Node *)tb_nn(v_n, 531))->q_name), ") = "));
    } else {
        f_emit__put(v_c, tb_cat(tb_cat("v_", ((tb_o_Node *)tb_nn(v_n, 533))->q_name), " = "));
    }
    if (((((tb_o_Node *)tb_nn(v_n, 534))->q_kids)->len == 1LL)) {
        f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 535))->q_kids, 0LL, sizeof(tb_o_Node *), 535)), ((tb_o_Node *)tb_nn(v_n, 535))->q_ty);
    } else {
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_n, 537))->q_ty);
    }
    f_emit__put(v_c, ";\n");
}

static void f_emit__emit_set(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 542))->q_kids, 0LL, sizeof(tb_o_Node *), 542));
    tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 543))->q_kids, 1LL, sizeof(tb_o_Node *), 543));
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 544))->q_value;
    const char * v_t = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 545))->q_ty);
    f_emit__emit_pre(v_c, v_value, v_depth);
    { long long tb_v264 = 1LL; long long *tb_p264 = &(((tb_o_Comp *)tb_nn(v_c, 547))->q_label); *tb_p264 += tb_v264; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 548))->q_label));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{ ");
    if (((strcmp(((tb_o_Node *)tb_nn(v_target, 551))->q_kind, "index") == 0) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 551))->q_kids, 0LL, sizeof(tb_o_Node *), 551)), 551))->q_ty))) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(v_t, " tb_v"), v_id), " = "));
        f_emit__emit_expr(v_c, v_value);
        f_emit__put(v_c, tb_cat(tb_cat("; tb_colset_", f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 555))->q_kids, 0LL, sizeof(tb_o_Node *), 555)), 555))->q_ty)), "("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 556))->q_kids, 0LL, sizeof(tb_o_Node *), 556)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 558))->q_kids, 1LL, sizeof(tb_o_Node *), 558)));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(", tb_v", v_id), ", "), tb_itos(((tb_o_Node *)tb_nn(v_n, 559))->q_line)), "); }\n"));
        return;
    }
    if (((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 561))->q_ty), "text") == 0) && (strcmp(v_op, "=") != 0))) {
        f_emit__put(v_c, tb_cat(tb_cat("const char *tb_v", v_id), " = "));
        f_emit__emit_as_text(v_c, v_value);
    } else {
        if ((strcmp(v_op, "=") == 0)) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(v_t, " tb_v"), v_id), " = "));
            f_emit__emit_as(v_c, v_value, ((tb_o_Node *)tb_nn(v_n, 567))->q_ty);
        } else {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_value, 569))->q_ty), " tb_v"), v_id), " = "));
            f_emit__emit_expr(v_c, v_value);
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("; ", v_t), " *tb_p"), v_id), " = &("));
    f_emit__emit_target(v_c, v_target);
    f_emit__put(v_c, "); ");
    if ((strcmp(v_op, "=") == 0)) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("*tb_p", v_id), " = tb_v"), v_id), ";"));
    } else if ((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 576))->q_ty), "text") == 0)) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("*tb_p", v_id), " = tb_cat(*tb_p"), v_id), ", tb_v"), v_id), ");"));
    } else if (((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 578))->q_ty), "int") == 0) && (strcmp(v_op, "/=") == 0))) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("*tb_p", v_id), " = tb_div(*tb_p"), v_id), ", tb_v"), v_id), ", "), tb_itos(((tb_o_Node *)tb_nn(v_n, 579))->q_line)), ");"));
    } else {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("*tb_p", v_id), " "), v_op), " tb_v"), v_id), ";"));
    }
    f_emit__put(v_c, " }\n");
}

static void f_emit__emit_multi(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    long long v_names = ((((tb_o_Node *)tb_nn(v_n, 585))->q_kids)->len - 1LL);
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 586))->q_kids, v_names, sizeof(tb_o_Node *), 586));
    f_emit__emit_pre(v_c, v_call, v_depth);
    { long long tb_v265 = 1LL; long long *tb_p265 = &(((tb_o_Comp *)tb_nn(v_c, 588))->q_label); *tb_p265 += tb_v265; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 589))->q_label));
    {
        long long tb_end266 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end266; v_i++) {
            if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 591))->q_kids, v_i, sizeof(tb_o_Node *), 591)), 591))->q_creates) {
                f_emit__indent(v_c, v_depth);
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 593))->q_kids, v_i, sizeof(tb_o_Node *), 593)), 593))->q_ty), " v_"), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 593))->q_kids, v_i, sizeof(tb_o_Node *), 593)), 593))->q_name), ";\n"));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_call, 597))->q_ty), " tb_m"), v_id), " = "));
    f_emit__emit_expr(v_c, v_call);
    f_emit__put(v_c, ";\n");
    {
        long long tb_end267 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end267; v_i++) {
            f_emit__indent(v_c, (v_depth + 1LL));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 602))->q_kids, v_i, sizeof(tb_o_Node *), 602)));
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(" = tb_m", v_id), ".o"), tb_itos(v_i)), ";\n"));
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_if(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    f_emit__indent(v_c, v_depth);
    long long v_i = 0LL;
    while (((v_i + 1LL) < (((tb_o_Node *)tb_nn(v_n, 610))->q_kids)->len)) {
        if ((v_i > 0LL)) {
            f_emit__put(v_c, " else ");
        }
        f_emit__put(v_c, "if (");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 614))->q_kids, v_i, sizeof(tb_o_Node *), 614)));
        f_emit__put(v_c, ") {\n");
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 616))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 616)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}");
        { long long tb_v268 = 2LL; long long *tb_p268 = &(v_i); *tb_p268 += tb_v268; }
    }
    if ((tb_mod((((tb_o_Node *)tb_nn(v_n, 620))->q_kids)->len, 2LL, 620) == 1LL)) {
        f_emit__put(v_c, " else {\n");
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 622))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 622))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 622)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}");
    }
    f_emit__put(v_c, "\n");
}

static void f_emit__emit_par(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v269 = 1LL; long long *tb_p269 = &(((tb_o_Comp *)tb_nn(v_c, 631))->q_label); *tb_p269 += tb_v269; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 632))->q_label));
    _Bool v_is_each = (strcmp(((tb_o_Node *)tb_nn(v_n, 633))->q_kind, "each") == 0);
    const char * v_elem = "";
    if (v_is_each) {
        v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 636))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 636))->q_kids, 0LL, sizeof(tb_o_Node *), 636)), 636))->q_ty, sizeof(tb_o_Type *), 636)), 636))->q_elem);
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 638))->q_out;
    tb_list * v_w = tb_list_new(sizeof(const char *));
    { tb_list * tb_v270 = v_w; tb_list * *tb_p270 = &(((tb_o_Comp *)tb_nn(v_c, 640))->q_out); *tb_p270 = tb_v270; }
    const char * v_list_type = "tb_list *";
    if (v_is_each) {
        v_list_type = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 643))->q_kids, 0LL, sizeof(tb_o_Node *), 643)), 643))->q_ty);
    }
    f_emit__put(v_c, tb_cat(tb_cat("\ntypedef struct {\n    long long tb_lo, tb_total, tb_step, tb_next;\n    ", v_list_type), " tb_l;\n"));
    {
        tb_list * tb_l271 = ((tb_o_Node *)tb_nn(v_n, 645))->q_caps;
        for (long long tb_i271 = 0; tb_i271 < tb_l271->len; tb_i271++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l271->data)[tb_i271];
            if (((tb_o_Var *)tb_nn(v_v, 646))->q_is_shared) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 647))->q_type)), " *p_"), ((tb_o_Var *)tb_nn(v_v, 647))->q_name), ";\n"));
            } else if (((tb_o_Var *)tb_nn(v_v, 648))->q_is_link) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 649))->q_type)), " *v_"), ((tb_o_Var *)tb_nn(v_v, 649))->q_name), ";\n"));
            } else {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 651))->q_type)), " v_"), ((tb_o_Var *)tb_nn(v_v, 651))->q_name), ";\n"));
            }
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat("} tb_job", v_id), ";\n"));
    f_emit__put(v_c, tb_cat(tb_cat("static void *tb_work", v_id), "(void *tb_arg) {\n"));
    f_emit__put(v_c, tb_cat(tb_cat("    tb_job", v_id), " *tb_j = tb_arg;\n"));
    _Bool v_has_shared = 0;
    {
        tb_list * tb_l272 = ((tb_o_Node *)tb_nn(v_n, 656))->q_caps;
        for (long long tb_i272 = 0; tb_i272 < tb_l272->len; tb_i272++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l272->data)[tb_i272];
            if (((tb_o_Var *)tb_nn(v_v, 657))->q_is_shared) {
                v_has_shared = 1;
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 659))->q_type)), " v_"), ((tb_o_Var *)tb_nn(v_v, 659))->q_name), " = "));
                f_emit__emit_default(v_c, ((tb_o_Var *)tb_nn(v_v, 660))->q_type);
                f_emit__put(v_c, ";\n");
            } else if (((tb_o_Var *)tb_nn(v_v, 662))->q_is_link) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 663))->q_type)), " *v_"), ((tb_o_Var *)tb_nn(v_v, 663))->q_name), " = tb_j->v_"), ((tb_o_Var *)tb_nn(v_v, 663))->q_name), ";\n"));
            } else {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 665))->q_type)), " v_"), ((tb_o_Var *)tb_nn(v_v, 665))->q_name), " = tb_j->v_"), ((tb_o_Var *)tb_nn(v_v, 665))->q_name), ";\n"));
            }
        }
    }
    f_emit__put(v_c, "    for (;;) {\n");
    f_emit__put(v_c, "        long long tb_s = __atomic_fetch_add(&tb_j->tb_next, tb_j->tb_step, __ATOMIC_RELAXED);\n");
    f_emit__put(v_c, "        if (tb_s >= tb_j->tb_total) break;\n");
    f_emit__put(v_c, "        long long tb_e = tb_s + tb_j->tb_step;\n");
    f_emit__put(v_c, "        if (tb_e > tb_j->tb_total) tb_e = tb_j->tb_total;\n");
    f_emit__put(v_c, "        for (long long tb_i = tb_s; tb_i < tb_e; tb_i++) {\n");
    if ((v_is_each && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 672))->q_kids, 0LL, sizeof(tb_o_Node *), 672)), 672))->q_ty))) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("            ", v_elem), " const v_"), ((tb_o_Node *)tb_nn(v_n, 673))->q_name), " = {tb_j->tb_l, tb_i};\n"));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 674))->q_kids, 1LL, sizeof(tb_o_Node *), 674)), 3LL);
    } else if (v_is_each) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("            ", v_elem), " const v_"), ((tb_o_Node *)tb_nn(v_n, 676))->q_name), " = (("), v_elem), " *)tb_j->tb_l->data)[tb_i];\n"));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 677))->q_kids, 1LL, sizeof(tb_o_Node *), 677)), 3LL);
    } else {
        f_emit__put(v_c, tb_cat(tb_cat("            long long const v_", ((tb_o_Node *)tb_nn(v_n, 679))->q_name), " = tb_j->tb_lo + tb_i;\n"));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 680))->q_kids, 2LL, sizeof(tb_o_Node *), 680)), 3LL);
    }
    f_emit__put(v_c, "        }\n    }\n");
    if (v_has_shared) {
        f_emit__put(v_c, "    pthread_mutex_lock(&tb_par_lock);\n");
        {
            tb_list * tb_l273 = ((tb_o_Node *)tb_nn(v_n, 684))->q_caps;
            for (long long tb_i273 = 0; tb_i273 < tb_l273->len; tb_i273++) {
                tb_o_Var * const v_v = ((tb_o_Var * *)tb_l273->data)[tb_i273];
                if (((tb_o_Var *)tb_nn(v_v, 685))->q_is_shared) {
                    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    *tb_j->p_", ((tb_o_Var *)tb_nn(v_v, 686))->q_name), " += v_"), ((tb_o_Var *)tb_nn(v_v, 686))->q_name), ";\n"));
                }
            }
        }
        f_emit__put(v_c, "    pthread_mutex_unlock(&tb_par_lock);\n");
    }
    f_emit__put(v_c, "    return 0;\n}\n");
    { const char * tb_v274 = tb_join(v_w, ""); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 689))->q_workers) = tb_v274; }
    { tb_list * tb_v275 = v_saved; tb_list * *tb_p275 = &(((tb_o_Comp *)tb_nn(v_c, 690))->q_out); *tb_p275 = tb_v275; }
    const char * v_j = tb_cat("tb_j", v_id);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("tb_job", v_id), " "), v_j), " = {0};\n"));
    f_emit__indent(v_c, (v_depth + 1LL));
    if (v_is_each) {
        f_emit__put(v_c, tb_cat(v_j, ".tb_l = "));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 700))->q_kids, 0LL, sizeof(tb_o_Node *), 700)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(v_j, ".tb_total = "), v_j), ".tb_l->len;\n"));
    } else {
        f_emit__put(v_c, tb_cat(tb_cat("long long tb_end", v_id), " = "));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 706))->q_kids, 1LL, sizeof(tb_o_Node *), 706)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, tb_cat(v_j, ".tb_lo = "));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 710))->q_kids, 0LL, sizeof(tb_o_Node *), 710)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_j, ".tb_total = tb_end"), v_id), " < "), v_j), ".tb_lo ? 0 : tb_end"), v_id), " - "), v_j), ".tb_lo + 1;\n"));
    }
    {
        tb_list * tb_l276 = ((tb_o_Node *)tb_nn(v_n, 714))->q_caps;
        for (long long tb_i276 = 0; tb_i276 < tb_l276->len; tb_i276++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l276->data)[tb_i276];
            f_emit__indent(v_c, (v_depth + 1LL));
            if (((tb_o_Var *)tb_nn(v_v, 716))->q_is_shared) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_j, ".p_"), ((tb_o_Var *)tb_nn(v_v, 717))->q_name), " = &v_"), ((tb_o_Var *)tb_nn(v_v, 717))->q_name), ";\n"));
            } else {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_j, ".v_"), ((tb_o_Var *)tb_nn(v_v, 719))->q_name), " = v_"), ((tb_o_Var *)tb_nn(v_v, 719))->q_name), ";\n"));
            }
        }
    }
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(v_j, ".tb_step = tb_par_step("), v_j), ".tb_total);\n"));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("tb_par_run(tb_work", v_id), ", &"), v_j), ", "), v_j), ".tb_total);\n"));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static long long f_emit__vec_index(tb_o_Comp * v_c, const char * v_name) {
    {
        long long tb_end277 = ((((tb_o_Comp *)tb_nn(v_c, 728))->q_vec_names)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end277; v_i++) {
            if ((strcmp((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 729))->q_vec_names, v_i, sizeof(const char *), 729)), v_name) == 0)) {
                return v_i;
            }
        }
    }
    return (-1LL);
    tb_crash(727, "функция vec_index дошла до конца без return");
    return 0LL;
}

static void f_emit__emit_vec(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v278 = 1LL; long long *tb_p278 = &(((tb_o_Comp *)tb_nn(v_c, 736))->q_label); *tb_p278 += tb_v278; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 737))->q_label));
    const char * v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 738))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 738))->q_kids, 0LL, sizeof(tb_o_Node *), 738)), 738))->q_ty, sizeof(tb_o_Type *), 738)), 738))->q_elem);
    long long v_d = (v_depth + 1LL);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, tb_cat(tb_cat("tb_list *tb_l", v_id), " = "));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 744))->q_kids, 0LL, sizeof(tb_o_Node *), 744)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_elem, " *tb_d"), v_id), " = ("), v_elem), " *)tb_l"), v_id), "->data;\n"));
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("long long tb_n", v_id), " = tb_l"), v_id), "->len, tb_i"), v_id), " = 0;\n"));
    {
        tb_list * tb_l279 = ((tb_o_Node *)tb_nn(v_n, 750))->q_caps;
        for (long long tb_i279 = 0; tb_i279 < tb_l279->len; tb_i279++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l279->data)[tb_i279];
            if (((tb_o_Var *)tb_nn(v_v, 751))->q_is_shared) {
                f_emit__indent(v_c, v_d);
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 753))->q_type), " tb_a"), v_id), "_"), ((tb_o_Var *)tb_nn(v_v, 753))->q_name), "[8] = {0};\n"));
            }
        }
    }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("for (; tb_i", v_id), " + 8 <= tb_n"), v_id), "; tb_i"), v_id), " += 8) {\n"));
    f_emit__indent(v_c, (v_d + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("for (int tb_k", v_id), " = 0; tb_k"), v_id), " < 8; tb_k"), v_id), "++) {\n"));
    f_emit__indent(v_c, (v_d + 2LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_elem, " const v_"), ((tb_o_Node *)tb_nn(v_n, 760))->q_name), " = tb_d"), v_id), "[tb_i"), v_id), " + tb_k"), v_id), "];\n"));
    {
        tb_list * tb_l280 = ((tb_o_Node *)tb_nn(v_n, 761))->q_caps;
        for (long long tb_i280 = 0; tb_i280 < tb_l280->len; tb_i280++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l280->data)[tb_i280];
            if (((tb_o_Var *)tb_nn(v_v, 762))->q_is_shared) {
                { const char * tb_v281 = ((tb_o_Var *)tb_nn(v_v, 763))->q_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 763))->q_vec_names) = tb_v281; }
                { const char * tb_v282 = tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("tb_a", v_id), "_"), ((tb_o_Var *)tb_nn(v_v, 764))->q_name), "[tb_k"), v_id), "]"); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 764))->q_vec_repl) = tb_v282; }
            }
        }
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 765))->q_kids, 1LL, sizeof(tb_o_Node *), 765)), (v_d + 2LL));
    tb_list * v_fresh_names = tb_list_new(sizeof(const char *));
    tb_list * v_fresh_repl = tb_list_new(sizeof(const char *));
    { tb_list * tb_v283 = v_fresh_names; tb_list * *tb_p283 = &(((tb_o_Comp *)tb_nn(v_c, 768))->q_vec_names); *tb_p283 = tb_v283; }
    { tb_list * tb_v284 = v_fresh_repl; tb_list * *tb_p284 = &(((tb_o_Comp *)tb_nn(v_c, 769))->q_vec_repl); *tb_p284 = tb_v284; }
    f_emit__indent(v_c, (v_d + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("for (; tb_i", v_id), " < tb_n"), v_id), "; tb_i"), v_id), "++) {\n"));
    f_emit__indent(v_c, (v_d + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_elem, " const v_"), ((tb_o_Node *)tb_nn(v_n, 778))->q_name), " = tb_d"), v_id), "[tb_i"), v_id), "];\n"));
    {
        tb_list * tb_l285 = ((tb_o_Node *)tb_nn(v_n, 779))->q_caps;
        for (long long tb_i285 = 0; tb_i285 < tb_l285->len; tb_i285++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l285->data)[tb_i285];
            if (((tb_o_Var *)tb_nn(v_v, 780))->q_is_shared) {
                { const char * tb_v286 = ((tb_o_Var *)tb_nn(v_v, 781))->q_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 781))->q_vec_names) = tb_v286; }
                { const char * tb_v287 = tb_cat(tb_cat(tb_cat(tb_cat("tb_a", v_id), "_"), ((tb_o_Var *)tb_nn(v_v, 782))->q_name), "[0]"); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 782))->q_vec_repl) = tb_v287; }
            }
        }
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 783))->q_kids, 1LL, sizeof(tb_o_Node *), 783)), (v_d + 1LL));
    tb_list * v_fresh_names2 = tb_list_new(sizeof(const char *));
    tb_list * v_fresh_repl2 = tb_list_new(sizeof(const char *));
    { tb_list * tb_v288 = v_fresh_names2; tb_list * *tb_p288 = &(((tb_o_Comp *)tb_nn(v_c, 786))->q_vec_names); *tb_p288 = tb_v288; }
    { tb_list * tb_v289 = v_fresh_repl2; tb_list * *tb_p289 = &(((tb_o_Comp *)tb_nn(v_c, 787))->q_vec_repl); *tb_p289 = tb_v289; }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, "}\n");
    {
        tb_list * tb_l290 = ((tb_o_Node *)tb_nn(v_n, 791))->q_caps;
        for (long long tb_i290 = 0; tb_i290 < tb_l290->len; tb_i290++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l290->data)[tb_i290];
            if (((tb_o_Var *)tb_nn(v_v, 792))->q_is_shared) {
                const char * v_target = tb_cat("v_", ((tb_o_Var *)tb_nn(v_v, 793))->q_name);
                if (((tb_o_Var *)tb_nn(v_v, 794))->q_is_link) {
                    v_target = tb_cat(tb_cat("(*v_", ((tb_o_Var *)tb_nn(v_v, 795))->q_name), ")");
                }
                f_emit__indent(v_c, v_d);
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("for (int tb_k = 0; tb_k < 8; tb_k++) ", v_target), " += tb_a"), v_id), "_"), ((tb_o_Var *)tb_nn(v_v, 797))->q_name), "[tb_k];\n"));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_each(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    if (((tb_o_Node *)tb_nn(v_n, 802))->q_by_vec) {
        f_emit__emit_vec(v_c, v_n, v_depth);
        return;
    }
    if (((tb_o_Node *)tb_nn(v_n, 805))->q_is_par) {
        f_emit__emit_par(v_c, v_n, v_depth);
        return;
    }
    if (((tb_o_Node *)tb_nn(v_n, 808))->q_by_piece) {
        f_emit__emit_pieces(v_c, v_n, v_depth);
        return;
    }
    { long long tb_v291 = 1LL; long long *tb_p291 = &(((tb_o_Comp *)tb_nn(v_c, 811))->q_label); *tb_p291 += tb_v291; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 812))->q_label));
    long long v_t = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 813))->q_kids, 0LL, sizeof(tb_o_Node *), 813)), 813))->q_ty;
    const char * v_k = f_check__kind_of(v_c, v_t);
    long long v_elem_ty = (-1LL);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    if ((strcmp(v_k, "dict") == 0)) {
        f_emit__put(v_c, tb_cat(tb_cat("tb_list * tb_l", v_id), " = ("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 821))->q_kids, 0LL, sizeof(tb_o_Node *), 821)));
        f_emit__put(v_c, ")->keys;\n");
        v_k = "list";
        v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 824))->q_types, v_t, sizeof(tb_o_Type *), 824)), 824))->q_key;
    } else {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, v_t), " tb_l"), v_id), " = "));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 827))->q_kids, 0LL, sizeof(tb_o_Node *), 827)));
        f_emit__put(v_c, ";\n");
        if ((strcmp(v_k, "list") == 0)) {
            v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 830))->q_types, v_t, sizeof(tb_o_Type *), 830)), 830))->q_elem;
        }
    }
    f_emit__indent(v_c, (v_depth + 1LL));
    if ((strcmp(v_k, "text") == 0)) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("for (long long tb_i", v_id), " = 0; tb_l"), v_id), "[tb_i"), v_id), "] != 0; tb_i"), v_id), "++) {\n"));
        f_emit__indent(v_c, (v_depth + 2LL));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("const char * const v_", ((tb_o_Node *)tb_nn(v_n, 835))->q_name), " = tb_chr((unsigned char)tb_l"), v_id), "[tb_i"), v_id), "]);\n"));
    } else {
        const char * v_elem = f_emit__c_type(v_c, v_elem_ty);
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("for (long long tb_i", v_id), " = 0; tb_i"), v_id), " < tb_l"), v_id), "->len; tb_i"), v_id), "++) {\n"));
        f_emit__indent(v_c, (v_depth + 2LL));
        if (f_check__is_cols_obj(v_c, v_elem_ty)) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_elem, " const v_"), ((tb_o_Node *)tb_nn(v_n, 841))->q_name), " = {tb_l"), v_id), ", tb_i"), v_id), "};\n"));
        } else {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(v_elem, " const v_"), ((tb_o_Node *)tb_nn(v_n, 843))->q_name), " = (("), v_elem), " *)tb_l"), v_id), "->data)[tb_i"), v_id), "];\n"));
        }
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 844))->q_kids, 1LL, sizeof(tb_o_Node *), 844)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_pieces(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v292 = 1LL; long long *tb_p292 = &(((tb_o_Comp *)tb_nn(v_c, 855))->q_label); *tb_p292 += tb_v292; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 856))->q_label));
    const char * v_l = tb_cat("tb_l", v_id);
    const char * v_i = tb_cat("tb_i", v_id);
    const char * v_p = tb_cat("tb_p", v_id);
    const char * v_m = tb_cat("tb_n", v_id);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat("tb_list *", v_l), " = "));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 865))->q_kids, 0LL, sizeof(tb_o_Node *), 865)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat("long long ", v_m), " = "));
    f_emit__emit_expr(v_c, ((tb_o_Node *)tb_nn(v_n, 869))->q_piece_size);
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("if (", v_m), " <= 0) tb_crash("), tb_itos(((tb_o_Node *)tb_nn(v_n, 872))->q_line)), ", \"размер куска в by должен быть больше 0\");\n"));
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
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("tb_list * const v_", ((tb_o_Node *)tb_nn(v_n, 888))->q_name), " = &"), v_p), ";\n"));
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 889))->q_kids, 1LL, sizeof(tb_o_Node *), 889)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(v_l, "->cap = tb_cap"), v_id), ";\n"));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_background(tb_o_Comp * v_c, tb_o_Node * v_n) {
    { long long tb_v293 = 1LL; long long *tb_p293 = &(((tb_o_Comp *)tb_nn(v_c, 901))->q_label); *tb_p293 += tb_v293; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 902))->q_label));
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 903))->q_kids, 0LL, sizeof(tb_o_Node *), 903));
    tb_o_Node * v_f = ((tb_o_Node *)tb_nn(v_call, 904))->q_ref;
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 905))->q_kids, 0LL, sizeof(tb_o_Node *), 905));
    long long v_rt = ((tb_o_Node *)tb_nn(v_f, 906))->q_ty;
    if ((v_rt < 0LL)) {
        v_rt = 0LL;
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 909))->q_out;
    tb_list * v_w = tb_list_new(sizeof(const char *));
    { tb_list * tb_v294 = v_w; tb_list * *tb_p294 = &(((tb_o_Comp *)tb_nn(v_c, 911))->q_out); *tb_p294 = tb_v294; }
    f_emit__put(v_c, "\ntypedef struct {\n    tb_task tb_t;\n");
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, tb_cat(tb_cat("    ", f_emit__c_type(v_c, v_rt)), " tb_r;\n"));
    }
    {
        long long tb_end295 = ((((tb_o_Node *)tb_nn(v_params, 915))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end295; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 916))->q_kids, v_i, sizeof(tb_o_Node *), 916)), 916))->q_ty)), " a"), tb_itos(v_i)), ";\n"));
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat("} tb_bgjob", v_id), ";\n"));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static void *tb_bgrun", v_id), "(void *tb_arg) {\n    tb_bgjob"), v_id), " *tb_j = tb_arg;\n    "));
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, "tb_j->tb_r = ");
    }
    f_emit__put(v_c, tb_cat(f_emit__fname(v_f), "("));
    {
        long long tb_end296 = ((((tb_o_Node *)tb_nn(v_params, 922))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end296; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, tb_cat("tb_j->a", tb_itos(v_i)));
        }
    }
    f_emit__put(v_c, ");\n    return 0;\n}\n");
    f_emit__put(v_c, tb_cat(tb_cat("static tb_task *tb_bg", v_id), "("));
    {
        long long tb_end297 = ((((tb_o_Node *)tb_nn(v_params, 928))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end297; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 931))->q_kids, v_i, sizeof(tb_o_Node *), 931)), 931))->q_ty), " a"), tb_itos(v_i)));
        }
    }
    if (((((tb_o_Node *)tb_nn(v_params, 932))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(") {\n    tb_bgjob", v_id), " *tb_j = malloc(sizeof(tb_bgjob"), v_id), "));\n"));
    f_emit__put(v_c, "    if (tb_j == 0) tb_crash(0, \"не хватило памяти\");\n");
    {
        long long tb_end298 = ((((tb_o_Node *)tb_nn(v_params, 936))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end298; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    tb_j->a", tb_itos(v_i)), " = a"), tb_itos(v_i)), ";\n"));
        }
    }
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, "    tb_j->tb_t.res = &tb_j->tb_r;\n");
    } else {
        f_emit__put(v_c, "    tb_j->tb_t.res = 0;\n");
    }
    f_emit__put(v_c, tb_cat(tb_cat("    tb_task_start(&tb_j->tb_t, tb_bgrun", v_id), ", tb_j);\n    return &tb_j->tb_t;\n}\n"));
    { tb_list * tb_v299 = v_saved; tb_list * *tb_p299 = &(((tb_o_Comp *)tb_nn(v_c, 943))->q_out); *tb_p299 = tb_v299; }
    { const char * tb_v300 = tb_join(v_w, ""); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 944))->q_workers) = tb_v300; }
    f_emit__put(v_c, tb_cat(tb_cat("tb_bg", v_id), "("));
    {
        long long tb_end301 = ((((tb_o_Node *)tb_nn(v_call, 946))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end301; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_call, 949))->q_kids, v_i, sizeof(tb_o_Node *), 949)), 949))->q_kids, 0LL, sizeof(tb_o_Node *), 949)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 949))->q_kids, v_i, sizeof(tb_o_Node *), 949)), 949))->q_ty);
        }
    }
    f_emit__put(v_c, ")");
}

static _Bool f_emit__is_pure(tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 954))->q_kind;
    if ((((((strcmp(v_k, "number") == 0) || (strcmp(v_k, "decimal") == 0)) || (strcmp(v_k, "string") == 0)) || (strcmp(v_k, "bool") == 0)) || (strcmp(v_k, "name") == 0))) {
        return 1;
    }
    if (((strcmp(v_k, "field") == 0) || (strcmp(v_k, "unary") == 0))) {
        return f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 958))->q_kids, 0LL, sizeof(tb_o_Node *), 958)));
    }
    if ((strcmp(v_k, "binop") == 0)) {
        if (((strcmp(((tb_o_Node *)tb_nn(v_n, 960))->q_value, "/") == 0) || (strcmp(((tb_o_Node *)tb_nn(v_n, 960))->q_value, "%") == 0))) {
            return 0;
        }
        return (f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 962))->q_kids, 0LL, sizeof(tb_o_Node *), 962))) && f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 962))->q_kids, 1LL, sizeof(tb_o_Node *), 962))));
    }
    return 0;
    tb_crash(953, "функция is_pure дошла до конца без return");
    return 0;
}

static void f_emit__emit_together(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v302 = 1LL; long long *tb_p302 = &(((tb_o_Comp *)tb_nn(v_c, 971))->q_label); *tb_p302 += tb_v302; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 972))->q_label));
    tb_o_Node * v_block = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 973))->q_kids, 0LL, sizeof(tb_o_Node *), 973));
    {
        long long tb_end303 = ((((tb_o_Node *)tb_nn(v_block, 974))->q_kids)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end303; v_j++) {
            tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_block, 975))->q_kids, v_j, sizeof(tb_o_Node *), 975)), 975))->q_kids, 0LL, sizeof(tb_o_Node *), 975));
            if ((((strcmp(((tb_o_Node *)tb_nn(v_v, 976))->q_kind, "index") != 0) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 976))->q_kids, 0LL, sizeof(tb_o_Node *), 976))))) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 976))->q_kids, 1LL, sizeof(tb_o_Node *), 976)))))) {
                continue;
            }
            f_emit__indent(v_c, v_depth);
            if ((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 979))->q_kids, 0LL, sizeof(tb_o_Node *), 979)), 979))->q_ty), "dict") == 0)) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("unsigned long long tb_h", v_id), "_"), tb_itos(v_j)), " = tb_dpre1("));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 981))->q_kids, 0LL, sizeof(tb_o_Node *), 981)));
                f_emit__put(v_c, ", ");
                f_emit__emit_key(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 983))->q_kids, 0LL, sizeof(tb_o_Node *), 983)), 983))->q_ty, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 983))->q_kids, 1LL, sizeof(tb_o_Node *), 983)));
                f_emit__put(v_c, ");\n");
            } else if ((f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 985))->q_kids, 0LL, sizeof(tb_o_Node *), 985)), 985))->q_ty) && (!f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 985))->q_kids, 0LL, sizeof(tb_o_Node *), 985)), 985))->q_ty)))) {
                f_emit__put(v_c, "tb_lpre(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 987))->q_kids, 0LL, sizeof(tb_o_Node *), 987)));
                f_emit__put(v_c, ", ");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 989))->q_kids, 1LL, sizeof(tb_o_Node *), 989)));
                f_emit__put(v_c, ");\n");
            } else {
                f_emit__put(v_c, ";\n");
            }
        }
    }
    {
        long long tb_end304 = ((((tb_o_Node *)tb_nn(v_block, 993))->q_kids)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end304; v_j++) {
            tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_block, 994))->q_kids, v_j, sizeof(tb_o_Node *), 994)), 994))->q_kids, 0LL, sizeof(tb_o_Node *), 994));
            if ((((strcmp(((tb_o_Node *)tb_nn(v_v, 995))->q_kind, "index") != 0) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 995))->q_kids, 0LL, sizeof(tb_o_Node *), 995))))) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 995))->q_kids, 1LL, sizeof(tb_o_Node *), 995)))))) {
                continue;
            }
            if ((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 997))->q_kids, 0LL, sizeof(tb_o_Node *), 997)), 997))->q_ty), "dict") == 0)) {
                f_emit__indent(v_c, v_depth);
                f_emit__put(v_c, "tb_dpre2(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1000))->q_kids, 0LL, sizeof(tb_o_Node *), 1000)));
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(", tb_h", v_id), "_"), tb_itos(v_j)), ");\n"));
            }
        }
    }
    {
        tb_list * tb_l305 = ((tb_o_Node *)tb_nn(v_block, 1002))->q_kids;
        for (long long tb_i305 = 0; tb_i305 < tb_l305->len; tb_i305++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l305->data)[tb_i305];
            f_emit__emit_statement(v_c, v_s, v_depth);
        }
    }
}

static void f_emit__emit_count(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    if (((tb_o_Node *)tb_nn(v_n, 1006))->q_is_par) {
        f_emit__emit_par(v_c, v_n, v_depth);
        return;
    }
    { long long tb_v306 = 1LL; long long *tb_p306 = &(((tb_o_Comp *)tb_nn(v_c, 1009))->q_label); *tb_p306 += tb_v306; }
    const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1010))->q_label));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat("long long tb_end", v_id), " = "));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1015))->q_kids, 1LL, sizeof(tb_o_Node *), 1015)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, tb_cat(tb_cat("for (long long v_", ((tb_o_Node *)tb_nn(v_n, 1018))->q_name), " = "));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1019))->q_kids, 0LL, sizeof(tb_o_Node *), 1019)));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("; v_", ((tb_o_Node *)tb_nn(v_n, 1020))->q_name), " <= tb_end"), v_id), "; v_"), ((tb_o_Node *)tb_nn(v_n, 1020))->q_name), "++) {\n"));
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1021))->q_kids, 2LL, sizeof(tb_o_Node *), 1021)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_gives_return(tb_o_Comp * v_c, long long v_depth) {
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1029))->q_cur_func, 1029))->q_kids, 2LL, sizeof(tb_o_Node *), 1029));
    f_emit__indent(v_c, v_depth);
    if (((((tb_o_Node *)tb_nn(v_outputs, 1031))->q_kids)->len == 1LL)) {
        f_emit__put(v_c, tb_cat(tb_cat("return v_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1032))->q_kids, 0LL, sizeof(tb_o_Node *), 1032)), 1032))->q_name), ";\n"));
        return;
    }
    f_emit__put(v_c, tb_cat(tb_cat("return (", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1034))->q_cur_func, 1034))->q_ty)), "){"));
    {
        long long tb_end307 = ((((tb_o_Node *)tb_nn(v_outputs, 1035))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end307; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, tb_cat("v_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1038))->q_kids, v_i, sizeof(tb_o_Node *), 1038)), 1038))->q_name));
        }
    }
    f_emit__put(v_c, "};\n");
}

static void f_emit__emit_statement(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1042))->q_kind;
    if ((strcmp(v_k, "assign") == 0)) {
        f_emit__emit_assign(v_c, v_n, v_depth);
    } else if ((strcmp(v_k, "set") == 0)) {
        f_emit__emit_set(v_c, v_n, v_depth);
    } else if ((strcmp(v_k, "linkdecl") == 0)) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 1049))->q_ty), " *v_"), ((tb_o_Node *)tb_nn(v_n, 1049))->q_name), " = &("));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1050))->q_kids, 0LL, sizeof(tb_o_Node *), 1050)));
        f_emit__put(v_c, ");\n");
    } else if ((strcmp(v_k, "multi") == 0)) {
        f_emit__emit_multi(v_c, v_n, v_depth);
    } else if ((strcmp(v_k, "if") == 0)) {
        f_emit__emit_if(v_c, v_n, v_depth);
    } else if ((strcmp(v_k, "each") == 0)) {
        f_emit__emit_each(v_c, v_n, v_depth);
    } else if ((strcmp(v_k, "count") == 0)) {
        f_emit__emit_count(v_c, v_n, v_depth);
    } else if ((strcmp(v_k, "repeat_times") == 0)) {
        { long long tb_v308 = 1LL; long long *tb_p308 = &(((tb_o_Comp *)tb_nn(v_c, 1061))->q_label); *tb_p308 += tb_v308; }
        const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1062))->q_label));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("for (long long tb_r", v_id), " = 0, tb_n"), v_id), " = "));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1065))->q_kids, 0LL, sizeof(tb_o_Node *), 1065)));
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("; tb_r", v_id), " < tb_n"), v_id), "; tb_r"), v_id), "++) {\n"));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1067))->q_kids, 1LL, sizeof(tb_o_Node *), 1067)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if ((strcmp(v_k, "repeat_while") == 0)) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "while (");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1073))->q_kids, 0LL, sizeof(tb_o_Node *), 1073)));
        f_emit__put(v_c, ") {\n");
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1075))->q_kids, 1LL, sizeof(tb_o_Node *), 1075)), (v_depth + 1LL));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if ((strcmp(v_k, "together") == 0)) {
        f_emit__emit_together(v_c, v_n, v_depth);
    } else if ((strcmp(v_k, "temp") == 0)) {
        { long long tb_v309 = 1LL; long long *tb_p309 = &(((tb_o_Comp *)tb_nn(v_c, 1081))->q_label); *tb_p309 += tb_v309; }
        const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1082))->q_label));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "{\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, tb_cat(tb_cat("tb_mark tb_k", v_id), " = tb_temp_begin();\n"));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1087))->q_kids, 0LL, sizeof(tb_o_Node *), 1087)), (v_depth + 1LL));
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, tb_cat(tb_cat("tb_temp_end(tb_k", v_id), ");\n"));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if ((strcmp(v_k, "skip") == 0)) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "continue;\n");
    } else if ((strcmp(v_k, "stop") == 0)) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "break;\n");
    } else if ((strcmp(v_k, "fail") == 0)) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "{ tb_err = ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1101))->q_kids, 0LL, sizeof(tb_o_Node *), 1101)));
        f_emit__put(v_c, "; ");
        f_emit__emit_bail(v_c);
        f_emit__put(v_c, " }\n");
    } else if ((strcmp(v_k, "return") == 0)) {
        if (((((tb_o_Node *)tb_nn(v_n, 1106))->q_kids)->len == 1LL)) {
            f_emit__emit_pre(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1107))->q_kids, 0LL, sizeof(tb_o_Node *), 1107)), v_depth);
        }
        if ((((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1108))->q_cur_func, 1108))->q_kids, 2LL, sizeof(tb_o_Node *), 1108)), 1108))->q_kids)->len > 0LL) && ((((tb_o_Node *)tb_nn(v_n, 1108))->q_kids)->len == 0LL))) {
            f_emit__emit_gives_return(v_c, v_depth);
        } else if (((((tb_o_Node *)tb_nn(v_n, 1110))->q_kids)->len == 1LL)) {
            f_emit__indent(v_c, v_depth);
            f_emit__put(v_c, "return ");
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1113))->q_kids, 0LL, sizeof(tb_o_Node *), 1113)), ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1113))->q_cur_func, 1113))->q_ty);
            f_emit__put(v_c, ";\n");
        } else {
            f_emit__indent(v_c, v_depth);
            f_emit__put(v_c, "return;\n");
        }
    } else if ((strcmp(v_k, "expr_stmt") == 0)) {
        tb_o_Node * v_e = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1119))->q_kids, 0LL, sizeof(tb_o_Node *), 1119));
        if (((strcmp(((tb_o_Node *)tb_nn(v_e, 1120))->q_kind, "try") == 0) || (strcmp(((tb_o_Node *)tb_nn(v_e, 1120))->q_kind, "else_err") == 0))) {
            f_emit__emit_pre(v_c, v_e, v_depth);
            return;
        }
        f_emit__indent(v_c, v_depth);
        if (((strcmp(((tb_o_Node *)tb_nn(v_e, 1124))->q_kind, "method") == 0) && (strcmp(((tb_o_Node *)tb_nn(v_e, 1124))->q_name, "add") == 0))) {
            long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1126))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1126))->q_kids, 0LL, sizeof(tb_o_Node *), 1126)), 1126))->q_ty, sizeof(tb_o_Type *), 1126)), 1126))->q_elem;
            { long long tb_v310 = 1LL; long long *tb_p310 = &(((tb_o_Comp *)tb_nn(v_c, 1127))->q_label); *tb_p310 += tb_v310; }
            const char * v_id = tb_cat("", tb_itos(((tb_o_Comp *)tb_nn(v_c, 1128))->q_label));
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1129))->q_kids, 1LL, sizeof(tb_o_Node *), 1129)), 1129))->q_kids, 0LL, sizeof(tb_o_Node *), 1129));
            if ((((f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1130))->q_kids, 0LL, sizeof(tb_o_Node *), 1130)), 1130))->q_ty) && (strcmp(((tb_o_Node *)tb_nn(v_value, 1130))->q_kind, "call") == 0)) && ((tb_o_Node *)tb_nn(v_value, 1130))->q_has_ref) && (strcmp(((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_value, 1130))->q_ref, 1130))->q_kind, "obj") == 0))) {
                tb_o_Node * v_o = ((tb_o_Node *)tb_nn(v_value, 1132))->q_ref;
                f_emit__put(v_c, tb_cat(tb_cat("tb_colpush_", ((tb_o_Node *)tb_nn(v_o, 1133))->q_name), "("));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1134))->q_kids, 0LL, sizeof(tb_o_Node *), 1134)));
                {
                    long long tb_end311 = ((((tb_o_Node *)tb_nn(v_o, 1135))->q_kids)->len - 1LL);
                    for (long long v_i = 0LL; v_i <= tb_end311; v_i++) {
                        f_emit__put(v_c, ", ");
                        if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 1137))->q_kids, v_i, sizeof(tb_o_Node *), 1137)), 1137))->q_kind, "missing") == 0)) {
                            f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1138))->q_kids, v_i, sizeof(tb_o_Node *), 1138)), 1138))->q_ty);
                        } else {
                            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 1140))->q_kids, v_i, sizeof(tb_o_Node *), 1140)), 1140))->q_kids, 0LL, sizeof(tb_o_Node *), 1140)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1140))->q_kids, v_i, sizeof(tb_o_Node *), 1140)), 1140))->q_ty);
                        }
                    }
                }
                f_emit__put(v_c, tb_cat(tb_cat(", ", tb_itos(((tb_o_Node *)tb_nn(v_e, 1141))->q_line)), ");\n"));
                return;
            }
            if (f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1143))->q_kids, 0LL, sizeof(tb_o_Node *), 1143)), 1143))->q_ty)) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("{ ", f_emit__c_type(v_c, v_elem)), " tb_v"), v_id), " = "));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1145))->q_kids, 1LL, sizeof(tb_o_Node *), 1145)), 1145))->q_kids, 0LL, sizeof(tb_o_Node *), 1145)));
                f_emit__put(v_c, tb_cat(tb_cat("; tb_coladd_", f_emit__cols_name(v_c, v_elem)), "("));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1147))->q_kids, 0LL, sizeof(tb_o_Node *), 1147)));
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(", tb_v", v_id), ", "), tb_itos(((tb_o_Node *)tb_nn(v_e, 1148))->q_line)), "); }\n"));
                return;
            }
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("{ ", f_emit__c_type(v_c, v_elem)), " tb_v"), v_id), " = "));
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1151))->q_kids, 1LL, sizeof(tb_o_Node *), 1151)), 1151))->q_kids, 0LL, sizeof(tb_o_Node *), 1151)), v_elem);
            f_emit__put(v_c, tb_cat(tb_cat("; *(", f_emit__c_type(v_c, v_elem)), " *)tb_grow("));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1153))->q_kids, 0LL, sizeof(tb_o_Node *), 1153)));
            f_emit__put(v_c, tb_cat(tb_cat(") = tb_v", v_id), "; }\n"));
        } else {
            f_emit__emit_expr(v_c, v_e);
            f_emit__put(v_c, ";\n");
        }
    }
}

static void f_emit__emit_block(tb_o_Comp * v_c, tb_o_Node * v_block, long long v_depth) {
    {
        tb_list * tb_l312 = ((tb_o_Node *)tb_nn(v_block, 1160))->q_kids;
        for (long long tb_i312 = 0; tb_i312 < tb_l312->len; tb_i312++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l312->data)[tb_i312];
            f_emit__emit_statement(v_c, v_s, v_depth);
        }
    }
}

static void f_emit__emit_signature(tb_o_Comp * v_c, tb_o_Node * v_f) {
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1166))->q_kids, 0LL, sizeof(tb_o_Node *), 1166));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1167))->q_ty)), " "), f_emit__fname(v_f)), "("));
    if (((((tb_o_Node *)tb_nn(v_params, 1168))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end313 = ((((tb_o_Node *)tb_nn(v_params, 1170))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end313; v_i++) {
            tb_o_Node * v_p = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1171))->q_kids, v_i, sizeof(tb_o_Node *), 1171));
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            if (((tb_o_Node *)tb_nn(v_p, 1174))->q_is_link) {
                f_emit__put(v_c, tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1175))->q_ty), " *v_"), ((tb_o_Node *)tb_nn(v_p, 1175))->q_name));
            } else {
                f_emit__put(v_c, tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1177))->q_ty), " v_"), ((tb_o_Node *)tb_nn(v_p, 1177))->q_name));
            }
        }
    }
    f_emit__put(v_c, ")");
}

static void f_emit__emit_func(tb_o_Comp * v_c, tb_o_Node * v_f) {
    { tb_o_Node * tb_v314 = v_f; tb_o_Node * *tb_p314 = &(((tb_o_Comp *)tb_nn(v_c, 1181))->q_cur_func); *tb_p314 = tb_v314; }
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1182))->q_kids, 2LL, sizeof(tb_o_Node *), 1182));
    f_emit__put(v_c, "\n");
    f_emit__emit_signature(v_c, v_f);
    f_emit__put(v_c, " {\n");
    {
        tb_list * tb_l315 = ((tb_o_Node *)tb_nn(v_outputs, 1186))->q_kids;
        for (long long tb_i315 = 0; tb_i315 < tb_l315->len; tb_i315++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l315->data)[tb_i315];
            if ((!((tb_o_Node *)tb_nn(v_o, 1187))->q_is_const)) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_o, 1188))->q_ty)), " v_"), ((tb_o_Node *)tb_nn(v_o, 1188))->q_name), " = "));
                f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_o, 1189))->q_ty);
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1191))->q_kids, 1LL, sizeof(tb_o_Node *), 1191)), 1LL);
    if (((((tb_o_Node *)tb_nn(v_outputs, 1192))->q_kids)->len > 0LL)) {
        f_emit__emit_gives_return(v_c, 1LL);
    } else if ((strcmp(f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 1194))->q_ty), "none") != 0)) {
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    tb_crash(", tb_itos(((tb_o_Node *)tb_nn(v_f, 1195))->q_line)), ", \"функция "), ((tb_o_Node *)tb_nn(v_f, 1195))->q_name), " дошла до конца без return\");\n"));
        f_emit__put(v_c, "    return ");
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_f, 1197))->q_ty);
        f_emit__put(v_c, ";\n");
    }
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_show(tb_o_Comp * v_c, tb_o_Type * v_t) {
    const char * v_k = ((tb_o_Type *)tb_nn(v_t, 1203))->q_kind;
    if ((((strcmp(v_k, "none") == 0) || (strcmp(v_k, "multi") == 0)) || (strcmp(v_k, "unknown") == 0))) {
        return;
    }
    if ((strcmp(v_k, "task") == 0)) {
        f_emit__put(v_c, tb_cat(tb_cat("static void tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1207))->q_id)), "(tb_task *x) { (void)x; tb_ps(\"<background>\"); }\n"));
        return;
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static void tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1209))->q_id)), "("), f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1209))->q_id)), " x) {\n"));
    if ((strcmp(v_k, "int") == 0)) {
        f_emit__put(v_c, "    printf(\"%lld\", x);\n");
    } else if ((strcmp(v_k, "dec") == 0)) {
        f_emit__put(v_c, "    printf(\"%g\", x);\n");
    } else if ((strcmp(v_k, "bool") == 0)) {
        f_emit__put(v_c, "    tb_ps(x ? \"true\" : \"false\");\n");
    } else if ((strcmp(v_k, "text") == 0)) {
        f_emit__put(v_c, "    tb_ps(x);\n");
    } else if ((strcmp(v_k, "list") == 0)) {
        const char * v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1219))->q_elem);
        f_emit__put(v_c, "    tb_ps(\"[\");\n");
        f_emit__put(v_c, "    for (long long i = 0; i < x->len; i++) {\n");
        f_emit__put(v_c, "        if (i > 0) tb_ps(\", \");\n");
        if (f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn(v_t, 1223))->q_elem)) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("        tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1224))->q_elem)), "(("), v_elem), "){x, i});\n"));
        } else {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("        tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1226))->q_elem)), "((("), v_elem), " *)x->data)[i]);\n"));
        }
        f_emit__put(v_c, "    }\n");
        f_emit__put(v_c, "    tb_ps(\"]\");\n");
    } else if ((strcmp(v_k, "dict") == 0)) {
        const char * v_kt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1230))->q_key);
        const char * v_vt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1231))->q_elem);
        f_emit__put(v_c, "    tb_ps(\"{\");\n");
        f_emit__put(v_c, "    for (long long i = 0; i < x->keys->len; i++) {\n");
        f_emit__put(v_c, "        if (i > 0) tb_ps(\", \");\n");
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("        tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1235))->q_key)), "((("), v_kt), " *)x->keys->data)[i]);\n"));
        f_emit__put(v_c, "        tb_ps(\": \");\n");
        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("        tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1237))->q_elem)), "((("), v_vt), " *)x->vals->data)[i]);\n"));
        f_emit__put(v_c, "    }\n");
        f_emit__put(v_c, "    tb_ps(\"}\");\n");
    } else if ((strcmp(v_k, "obj") == 0)) {
        tb_o_Node * v_o = ((tb_o_Type *)tb_nn(v_t, 1241))->q_obj_node;
        if (((tb_o_Node *)tb_nn(v_o, 1242))->q_by_cols) {
            f_emit__put(v_c, "    if (x.c == 0) { tb_ps(\"пусто\"); return; }\n");
        } else {
            f_emit__put(v_c, "    if (x == 0) { tb_ps(\"пусто\"); return; }\n");
        }
        f_emit__put(v_c, tb_cat(tb_cat("    tb_ps(\"", ((tb_o_Node *)tb_nn(v_o, 1246))->q_name), "(\");\n"));
        {
            long long tb_end316 = ((((tb_o_Node *)tb_nn(v_o, 1247))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end316; v_i++) {
                const char * v_sep = "";
                if ((v_i > 0LL)) {
                    v_sep = ", ";
                }
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat("    tb_ps(\"", v_sep), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1251))->q_kids, v_i, sizeof(tb_o_Node *), 1251)), 1251))->q_name), ": \");\n"));
                if (((tb_o_Node *)tb_nn(v_o, 1252))->q_by_cols) {
                    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    tb_show_", tb_itos(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1253))->q_kids, v_i, sizeof(tb_o_Node *), 1253)), 1253))->q_ty)), "(x.c->q_"), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1253))->q_kids, v_i, sizeof(tb_o_Node *), 1253)), 1253))->q_name), "[x.i]);\n"));
                } else {
                    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    tb_show_", tb_itos(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1255))->q_kids, v_i, sizeof(tb_o_Node *), 1255)), 1255))->q_ty)), "(x->q_"), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1255))->q_kids, v_i, sizeof(tb_o_Node *), 1255)), 1255))->q_name), ");\n"));
                }
            }
        }
        f_emit__put(v_c, "    tb_ps(\")\");\n");
    }
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_cols_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    f_emit__put(v_c, tb_cat(tb_cat("struct tb_cols_", ((tb_o_Node *)tb_nn(v_o, 1263))->q_name), " {\n    long long len, cap;\n"));
    {
        tb_list * tb_l317 = ((tb_o_Node *)tb_nn(v_o, 1264))->q_kids;
        for (long long tb_i317 = 0; tb_i317 < tb_l317->len; tb_i317++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l317->data)[tb_i317];
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1265))->q_ty)), " *q_"), ((tb_o_Node *)tb_nn(v_f, 1265))->q_name), ";\n"));
        }
    }
    f_emit__put(v_c, "};\n");
}

static void f_emit__emit_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((tb_o_Node *)tb_nn(v_o, 1269))->q_by_cols) {
        f_emit__emit_cols_obj(v_c, v_o);
        return;
    }
    f_emit__put(v_c, tb_cat(tb_cat("struct tb_o_", ((tb_o_Node *)tb_nn(v_o, 1272))->q_name), " {\n"));
    {
        tb_list * tb_l318 = ((tb_o_Node *)tb_nn(v_o, 1273))->q_kids;
        for (long long tb_i318 = 0; tb_i318 < tb_l318->len; tb_i318++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l318->data)[tb_i318];
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1274))->q_ty)), " q_"), ((tb_o_Node *)tb_nn(v_f, 1274))->q_name), ";\n"));
        }
    }
    if (((((tb_o_Node *)tb_nn(v_o, 1275))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "    char empty;\n");
    }
    f_emit__put(v_c, "};\n");
}

static void f_emit__emit_cols_helpers(tb_o_Comp * v_c, tb_o_Node * v_o) {
    const char * v_nm = ((tb_o_Node *)tb_nn(v_o, 1281))->q_name;
    const char * v_cols = tb_cat("tb_cols_", v_nm);
    const char * v_row = tb_cat("tb_row_", v_nm);
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static ", v_cols), " *tb_cols_new_"), v_nm), "(void) {\n"));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", v_cols), " *l = tb_mem(sizeof("), v_cols), "));\n    l->len = 0; l->cap = 4;\n"));
    {
        tb_list * tb_l319 = ((tb_o_Node *)tb_nn(v_o, 1286))->q_kids;
        for (long long tb_i319 = 0; tb_i319 < tb_l319->len; tb_i319++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l319->data)[tb_i319];
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    l->q_", ((tb_o_Node *)tb_nn(v_f, 1287))->q_name), " = tb_mem(sizeof("), f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1287))->q_ty)), ") * 4);\n"));
        }
    }
    f_emit__put(v_c, "    return l;\n}\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static void tb_colgrow_", v_nm), "("), v_cols), " *l) {\n"));
    f_emit__put(v_c, "    if (l->len < l->cap) return;\n    l->cap *= 2;\n");
    {
        tb_list * tb_l320 = ((tb_o_Node *)tb_nn(v_o, 1291))->q_kids;
        for (long long tb_i320 = 0; tb_i320 < tb_l320->len; tb_i320++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l320->data)[tb_i320];
            const char * v_ft = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1292))->q_ty);
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("    l->q_", ((tb_o_Node *)tb_nn(v_f, 1293))->q_name), " = tb_resize(l->q_"), ((tb_o_Node *)tb_nn(v_f, 1293))->q_name), ", sizeof("), v_ft), ") * (l->cap / 2), sizeof("), v_ft), ") * l->cap);\n"));
        }
    }
    f_emit__put(v_c, "}\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("static ", v_row), " tb_colat_"), v_nm), "("), v_cols), " *l, long long i, int line) {\n"));
    f_emit__put(v_c, tb_cat(tb_cat("    if (TB_RARE((unsigned long long)i >= (unsigned long long)((", v_cols), " *)tb_nn(l, line))->len)) tb_crash(line, \"номер за пределами списка\");\n"));
    f_emit__put(v_c, tb_cat(tb_cat("    return (", v_row), "){l, i};\n}\n"));
    {
        tb_list * tb_l321 = ((tb_o_Node *)tb_nn(v_o, 1298))->q_kids;
        for (long long tb_i321 = 0; tb_i321 < tb_l321->len; tb_i321++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l321->data)[tb_i321];
            const char * v_ft = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1299))->q_ty);
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("static inline ", v_ft), " *tb_colf_"), v_nm), "_"), ((tb_o_Node *)tb_nn(v_f, 1300))->q_name), "("), v_row), " r, int line) {\n"));
            f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
            f_emit__put(v_c, tb_cat(tb_cat("    return &r.c->q_", ((tb_o_Node *)tb_nn(v_f, 1302))->q_name), "[r.i];\n}\n"));
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("static void tb_coladd_", v_nm), "("), v_cols), " *l, "), v_row), " r, int line) {\n"));
    f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
    {
        long long tb_end322 = ((((tb_o_Node *)tb_nn(v_o, 1305))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end322; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("    ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1306))->q_kids, v_i, sizeof(tb_o_Node *), 1306)), 1306))->q_ty)), " a"), tb_itos(v_i)), " = r.c->q_"), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1306))->q_kids, v_i, sizeof(tb_o_Node *), 1306)), 1306))->q_name), "[r.i];\n"));
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat("    tb_colgrow_", v_nm), "(tb_nn(l, line));\n"));
    {
        long long tb_end323 = ((((tb_o_Node *)tb_nn(v_o, 1308))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end323; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    l->q_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1309))->q_kids, v_i, sizeof(tb_o_Node *), 1309)), 1309))->q_name), "[l->len] = a"), tb_itos(v_i)), ";\n"));
        }
    }
    f_emit__put(v_c, "    l->len += 1;\n}\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static void tb_colpush_", v_nm), "("), v_cols), " *l"));
    {
        long long tb_end324 = ((((tb_o_Node *)tb_nn(v_o, 1312))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end324; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(", ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1313))->q_kids, v_i, sizeof(tb_o_Node *), 1313)), 1313))->q_ty)), " a"), tb_itos(v_i)));
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat(", int line) {\n    tb_colgrow_", v_nm), "(tb_nn(l, line));\n"));
    {
        long long tb_end325 = ((((tb_o_Node *)tb_nn(v_o, 1315))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end325; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    l->q_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1316))->q_kids, v_i, sizeof(tb_o_Node *), 1316)), 1316))->q_name), "[l->len] = a"), tb_itos(v_i)), ";\n"));
        }
    }
    f_emit__put(v_c, "    l->len += 1;\n}\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("static void tb_colset_", v_nm), "("), v_cols), " *l, long long i, "), v_row), " r, int line) {\n"));
    f_emit__put(v_c, tb_cat(tb_cat("    tb_colat_", v_nm), "(l, i, line);\n"));
    f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
    {
        tb_list * tb_l326 = ((tb_o_Node *)tb_nn(v_o, 1321))->q_kids;
        for (long long tb_i326 = 0; tb_i326 < tb_l326->len; tb_i326++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l326->data)[tb_i326];
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    l->q_", ((tb_o_Node *)tb_nn(v_f, 1322))->q_name), "[i] = r.c->q_"), ((tb_o_Node *)tb_nn(v_f, 1322))->q_name), "[r.i];\n"));
        }
    }
    f_emit__put(v_c, "}\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(tb_cat(tb_cat("static ", v_cols), " *tb_colsfrom_"), v_nm), "(long long n, "), v_row), " *rows) {\n"));
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    ", v_cols), " *l = tb_cols_new_"), v_nm), "();\n"));
    f_emit__put(v_c, tb_cat(tb_cat("    for (long long i = 0; i < n; i++) tb_coladd_", v_nm), "(l, rows[i], 0);\n"));
    f_emit__put(v_c, "    return l;\n}\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static ", v_row), " tb_new_"), v_nm), "("));
    if (((((tb_o_Node *)tb_nn(v_o, 1330))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end327 = ((((tb_o_Node *)tb_nn(v_o, 1332))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end327; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1335))->q_kids, v_i, sizeof(tb_o_Node *), 1335)), 1335))->q_ty), " a"), tb_itos(v_i)));
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(") {\n    ", v_cols), " *l = tb_cols_new_"), v_nm), "();\n"));
    {
        long long tb_end328 = ((((tb_o_Node *)tb_nn(v_o, 1337))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end328; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    l->q_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1338))->q_kids, v_i, sizeof(tb_o_Node *), 1338)), 1338))->q_name), "[0] = a"), tb_itos(v_i)), ";\n"));
        }
    }
    f_emit__put(v_c, tb_cat(tb_cat("    l->len = 1;\n    return (", v_row), "){l, 0};\n}\n"));
}

static void f_emit__emit_constructor(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((tb_o_Node *)tb_nn(v_o, 1342))->q_by_cols) {
        f_emit__emit_cols_helpers(v_c, v_o);
        return;
    }
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static tb_o_", ((tb_o_Node *)tb_nn(v_o, 1345))->q_name), " *tb_new_"), ((tb_o_Node *)tb_nn(v_o, 1345))->q_name), "("));
    if (((((tb_o_Node *)tb_nn(v_o, 1346))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end329 = ((((tb_o_Node *)tb_nn(v_o, 1348))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end329; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, tb_cat(tb_cat(f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1351))->q_kids, v_i, sizeof(tb_o_Node *), 1351)), 1351))->q_ty), " a"), tb_itos(v_i)));
        }
    }
    f_emit__put(v_c, ") {\n");
    f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    tb_o_", ((tb_o_Node *)tb_nn(v_o, 1353))->q_name), " *o = tb_mem(sizeof(tb_o_"), ((tb_o_Node *)tb_nn(v_o, 1353))->q_name), "));\n"));
    {
        long long tb_end330 = ((((tb_o_Node *)tb_nn(v_o, 1354))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end330; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("    o->q_", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1355))->q_kids, v_i, sizeof(tb_o_Node *), 1355)), 1355))->q_name), " = a"), tb_itos(v_i)), ";\n"));
        }
    }
    f_emit__put(v_c, "    return o;\n}\n");
}

static void f_emit__emit_c(tb_o_Comp * v_c, tb_o_Node * v_program) {
    tb_list * v_fresh = tb_list_new(sizeof(const char *));
    { tb_list * tb_v331 = v_fresh; tb_list * *tb_p331 = &(((tb_o_Comp *)tb_nn(v_c, 1360))->q_out); *tb_p331 = tb_v331; }
    { long long tb_v332 = 0LL; long long *tb_p332 = &(((tb_o_Comp *)tb_nn(v_c, 1361))->q_label); *tb_p332 = tb_v332; }
    f_emit__put(v_c, "// Сделано компилятором Tachbit 0.7. Не правь руками: правь файл .tb\n");
    f_emit__put(v_c, f_runtime_text__runtime_text());
    {
        tb_list * tb_l333 = ((tb_o_Node *)tb_nn(v_program, 1366))->q_kids;
        for (long long tb_i333 = 0; tb_i333 < tb_l333->len; tb_i333++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l333->data)[tb_i333];
            if (((strcmp(((tb_o_Node *)tb_nn(v_top, 1367))->q_kind, "obj") == 0) && ((tb_o_Node *)tb_nn(v_top, 1367))->q_by_cols)) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("typedef struct tb_cols_", ((tb_o_Node *)tb_nn(v_top, 1368))->q_name), " tb_cols_"), ((tb_o_Node *)tb_nn(v_top, 1368))->q_name), ";\n"));
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("typedef struct { tb_cols_", ((tb_o_Node *)tb_nn(v_top, 1369))->q_name), " *c; long long i; } tb_row_"), ((tb_o_Node *)tb_nn(v_top, 1369))->q_name), ";\n"));
            } else if ((strcmp(((tb_o_Node *)tb_nn(v_top, 1370))->q_kind, "obj") == 0)) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("typedef struct tb_o_", ((tb_o_Node *)tb_nn(v_top, 1371))->q_name), " tb_o_"), ((tb_o_Node *)tb_nn(v_top, 1371))->q_name), ";\n"));
            }
        }
    }
    {
        tb_list * tb_l334 = ((tb_o_Node *)tb_nn(v_program, 1372))->q_kids;
        for (long long tb_i334 = 0; tb_i334 < tb_l334->len; tb_i334++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l334->data)[tb_i334];
            if ((strcmp(((tb_o_Node *)tb_nn(v_top, 1373))->q_kind, "obj") == 0)) {
                f_emit__emit_obj(v_c, v_top);
            }
        }
    }
    {
        tb_list * tb_l335 = ((tb_o_Node *)tb_nn(v_program, 1376))->q_kids;
        for (long long tb_i335 = 0; tb_i335 < tb_l335->len; tb_i335++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l335->data)[tb_i335];
            if (((strcmp(((tb_o_Node *)tb_nn(v_top, 1377))->q_kind, "func") == 0) && ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1377))->q_kids, 2LL, sizeof(tb_o_Node *), 1377)), 1377))->q_kids)->len > 1LL))) {
                f_emit__put(v_c, "typedef struct {");
                {
                    long long tb_end336 = ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1379))->q_kids, 2LL, sizeof(tb_o_Node *), 1379)), 1379))->q_kids)->len - 1LL);
                    for (long long v_j = 0LL; v_j <= tb_end336; v_j++) {
                        f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat(" ", f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1380))->q_kids, 2LL, sizeof(tb_o_Node *), 1380)), 1380))->q_kids, v_j, sizeof(tb_o_Node *), 1380)), 1380))->q_ty)), " o"), tb_itos(v_j)), ";"));
                    }
                }
                f_emit__put(v_c, tb_cat(tb_cat(" } tb_r_", f_emit__fname(v_top)), ";\n"));
            }
        }
    }
    {
        tb_list * tb_l337 = ((tb_o_Comp *)tb_nn(v_c, 1382))->q_types;
        for (long long tb_i337 = 0; tb_i337 < tb_l337->len; tb_i337++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l337->data)[tb_i337];
            const char * v_k = ((tb_o_Type *)tb_nn(v_t, 1383))->q_kind;
            if ((((strcmp(v_k, "none") != 0) && (strcmp(v_k, "multi") != 0)) && (strcmp(v_k, "unknown") != 0))) {
                f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static void tb_show_", tb_itos(((tb_o_Type *)tb_nn(v_t, 1385))->q_id)), "("), f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1385))->q_id)), " x);\n"));
            }
        }
    }
    {
        long long tb_end338 = ((((tb_o_Comp *)tb_nn(v_c, 1386))->q_temp_types)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end338; v_i++) {
            f_emit__put(v_c, tb_cat(tb_cat(tb_cat(tb_cat("static _Thread_local ", f_emit__c_type(v_c, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1387))->q_temp_types, v_i, sizeof(long long), 1387)))), " tb_t"), tb_itos(v_i)), ";   // для  f() or запасное\n"));
        }
    }
    {
        tb_list * tb_l339 = ((tb_o_Node *)tb_nn(v_program, 1388))->q_kids;
        for (long long tb_i339 = 0; tb_i339 < tb_l339->len; tb_i339++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l339->data)[tb_i339];
            if ((strcmp(((tb_o_Node *)tb_nn(v_top, 1389))->q_kind, "obj") == 0)) {
                f_emit__emit_constructor(v_c, v_top);
            }
        }
    }
    {
        tb_list * tb_l340 = ((tb_o_Node *)tb_nn(v_program, 1391))->q_kids;
        for (long long tb_i340 = 0; tb_i340 < tb_l340->len; tb_i340++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l340->data)[tb_i340];
            if ((strcmp(((tb_o_Node *)tb_nn(v_top, 1392))->q_kind, "func") == 0)) {
                f_emit__emit_signature(v_c, v_top);
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__put(v_c, "\n");
    {
        tb_list * tb_l341 = ((tb_o_Comp *)tb_nn(v_c, 1396))->q_types;
        for (long long tb_i341 = 0; tb_i341 < tb_l341->len; tb_i341++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l341->data)[tb_i341];
            f_emit__emit_show(v_c, v_t);
        }
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 1399))->q_out;
    tb_list * v_funcs_out = tb_list_new(sizeof(const char *));
    { tb_list * tb_v342 = v_funcs_out; tb_list * *tb_p342 = &(((tb_o_Comp *)tb_nn(v_c, 1401))->q_out); *tb_p342 = tb_v342; }
    {
        tb_list * tb_l343 = ((tb_o_Node *)tb_nn(v_program, 1402))->q_kids;
        for (long long tb_i343 = 0; tb_i343 < tb_l343->len; tb_i343++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l343->data)[tb_i343];
            if ((strcmp(((tb_o_Node *)tb_nn(v_top, 1403))->q_kind, "func") == 0)) {
                f_emit__emit_func(v_c, v_top);
            }
        }
    }
    { tb_list * tb_v344 = v_saved; tb_list * *tb_p344 = &(((tb_o_Comp *)tb_nn(v_c, 1405))->q_out); *tb_p344 = tb_v344; }
    {
        tb_list * tb_l345 = ((tb_o_Comp *)tb_nn(v_c, 1406))->q_workers;
        for (long long tb_i345 = 0; tb_i345 < tb_l345->len; tb_i345++) {
            const char * const v_w = ((const char * *)tb_l345->data)[tb_i345];
            f_emit__put(v_c, v_w);
        }
    }
    {
        tb_list * tb_l346 = v_funcs_out;
        for (long long tb_i346 = 0; tb_i346 < tb_l346->len; tb_i346++) {
            const char * const v_f = ((const char * *)tb_l346->data)[tb_i346];
            f_emit__put(v_c, v_f);
        }
    }
    tb_o_Node * v_main_func = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1410))->q_kids, 0LL, sizeof(tb_o_Node *), 1410));
    {
        tb_list * tb_l347 = ((tb_o_Node *)tb_nn(v_program, 1411))->q_kids;
        for (long long tb_i347 = 0; tb_i347 < tb_l347->len; tb_i347++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l347->data)[tb_i347];
            if ((((strcmp(((tb_o_Node *)tb_nn(v_top, 1412))->q_kind, "func") == 0) && (strcmp(((tb_o_Node *)tb_nn(v_top, 1412))->q_name, "main") == 0)) && (strcmp(((tb_o_Node *)tb_nn(v_top, 1412))->q_mod, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1412))->q_kids, 0LL, sizeof(tb_o_Node *), 1412)), 1412))->q_mod) == 0))) {
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
    { const char * tb_v348 = "// Библиотека, которую Tachbit кладёт в начало каждой программы на C.\n"; *(const char * *)tb_grow(v_lines) = tb_v348; }
    { const char * tb_v349 = "// Это обычный C. Компилятор на Tachbit встраивает этот текст через runtime_text.tb,\n"; *(const char * *)tb_grow(v_lines) = tb_v349; }
    { const char * tb_v350 = "// который делает tools/embed_runtime.py. Поменял этот файл — запусти:\n"; *(const char * *)tb_grow(v_lines) = tb_v350; }
    { const char * tb_v351 = "//     python3 tools/embed_runtime.py\n"; *(const char * *)tb_grow(v_lines) = tb_v351; }
    { const char * tb_v352 = "#include <stdio.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v352; }
    { const char * tb_v353 = "#include <stdlib.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v353; }
    { const char * tb_v354 = "#include <string.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v354; }
    { const char * tb_v355 = "#include <math.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v355; }
    { const char * tb_v356 = "#include <pthread.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v356; }
    { const char * tb_v357 = "#include <unistd.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v357; }
    { const char * tb_v358 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v358; }
    { const char * tb_v359 = "// подсказки для clang / gcc: ошибка бывает редко, а tb_crash не возвращается\n"; *(const char * *)tb_grow(v_lines) = tb_v359; }
    { const char * tb_v360 = "#if defined(__GNUC__)\n"; *(const char * *)tb_grow(v_lines) = tb_v360; }
    { const char * tb_v361 = "#define TB_RARE(x) __builtin_expect(!!(x), 0)\n"; *(const char * *)tb_grow(v_lines) = tb_v361; }
    { const char * tb_v362 = "#define TB_COLD __attribute__((cold, noreturn, noinline))\n"; *(const char * *)tb_grow(v_lines) = tb_v362; }
    { const char * tb_v363 = "#else\n"; *(const char * *)tb_grow(v_lines) = tb_v363; }
    { const char * tb_v364 = "#define TB_RARE(x) (x)\n"; *(const char * *)tb_grow(v_lines) = tb_v364; }
    { const char * tb_v365 = "#define TB_COLD\n"; *(const char * *)tb_grow(v_lines) = tb_v365; }
    { const char * tb_v366 = "#endif\n"; *(const char * *)tb_grow(v_lines) = tb_v366; }
    { const char * tb_v367 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v367; }
    { const char * tb_v368 = "// список: кусок памяти, len элементов по size байт, место на cap\n"; *(const char * *)tb_grow(v_lines) = tb_v368; }
    { const char * tb_v369 = "typedef struct { char *data; long long len; long long cap; long long size; } tb_list;\n"; *(const char * *)tb_grow(v_lines) = tb_v369; }
    { const char * tb_v370 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v370; }
    { const char * tb_v371 = "TB_COLD static void tb_crash(int line, const char *msg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v371; }
    { const char * tb_v372 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v372; }
    { const char * tb_v373 = "    // в терминале — красным (если не задан NO_COLOR)\n"; *(const char * *)tb_grow(v_lines) = tb_v373; }
    { const char * tb_v374 = "    int color = isatty(2) && getenv(\"NO_COLOR\") == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v374; }
    { const char * tb_v375 = "    if (color) fputs(\"\\033[31m\", stderr);\n"; *(const char * *)tb_grow(v_lines) = tb_v375; }
    { const char * tb_v376 = "    if (line > 0) fprintf(stderr, \"строка %d: %s\", line, msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v376; }
    { const char * tb_v377 = "    else fprintf(stderr, \"%s\", msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v377; }
    { const char * tb_v378 = "    fputs(color ? \"\\033[0m\\n\" : \"\\n\", stderr);\n"; *(const char * *)tb_grow(v_lines) = tb_v378; }
    { const char * tb_v379 = "    exit(1);\n"; *(const char * *)tb_grow(v_lines) = tb_v379; }
    { const char * tb_v380 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v380; }
    { const char * tb_v381 = "// ---------- temp: временная память ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v381; }
    { const char * tb_v382 = "// Внутри блока temp память берётся из больших кусков подряд (быстро, без malloc),\n"; *(const char * *)tb_grow(v_lines) = tb_v382; }
    { const char * tb_v383 = "// а в конце блока все куски отдаются разом. У каждого ядра свои куски.\n"; *(const char * *)tb_grow(v_lines) = tb_v383; }
    { const char * tb_v384 = "typedef struct { char *mem; long long size, used; } tb_chunk;\n"; *(const char * *)tb_grow(v_lines) = tb_v384; }
    { const char * tb_v385 = "typedef struct { int n; long long used; } tb_mark;\n"; *(const char * *)tb_grow(v_lines) = tb_v385; }
    { const char * tb_v386 = "static _Thread_local tb_chunk *tb_chunks = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v386; }
    { const char * tb_v387 = "static _Thread_local int tb_nchunks = 0, tb_capchunks = 0, tb_temp_depth = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v387; }
    { const char * tb_v388 = "static int tb_in_temp(const void *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v388; }
    { const char * tb_v389 = "    for (int i = tb_nchunks - 1; i >= 0; i--) {\n"; *(const char * *)tb_grow(v_lines) = tb_v389; }
    { const char * tb_v390 = "        if ((const char *)p >= tb_chunks[i].mem && (const char *)p < tb_chunks[i].mem + tb_chunks[i].size) return 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v390; }
    { const char * tb_v391 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v391; }
    { const char * tb_v392 = "    return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v392; }
    { const char * tb_v393 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v393; }
    { const char * tb_v394 = "static void *tb_temp_alloc(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v394; }
    { const char * tb_v395 = "    n = n > 0 ? (n + 15) & ~15LL : 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v395; }
    { const char * tb_v396 = "    tb_chunk *c = tb_nchunks > 0 ? &tb_chunks[tb_nchunks - 1] : 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v396; }
    { const char * tb_v397 = "    if (c == 0 || c->used + n > c->size) {\n"; *(const char * *)tb_grow(v_lines) = tb_v397; }
    { const char * tb_v398 = "        long long size = c != 0 ? c->size * 2 : (1LL << 20);\n"; *(const char * *)tb_grow(v_lines) = tb_v398; }
    { const char * tb_v399 = "        if (size < n) size = n;\n"; *(const char * *)tb_grow(v_lines) = tb_v399; }
    { const char * tb_v400 = "        if (tb_nchunks == tb_capchunks) {\n"; *(const char * *)tb_grow(v_lines) = tb_v400; }
    { const char * tb_v401 = "            tb_capchunks = tb_capchunks * 2 + 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v401; }
    { const char * tb_v402 = "            tb_chunks = realloc(tb_chunks, sizeof(tb_chunk) * tb_capchunks);\n"; *(const char * *)tb_grow(v_lines) = tb_v402; }
    { const char * tb_v403 = "            if (tb_chunks == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v403; }
    { const char * tb_v404 = "        }\n"; *(const char * *)tb_grow(v_lines) = tb_v404; }
    { const char * tb_v405 = "        char *mem = malloc(size);\n"; *(const char * *)tb_grow(v_lines) = tb_v405; }
    { const char * tb_v406 = "        if (mem == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v406; }
    { const char * tb_v407 = "        tb_chunks[tb_nchunks] = (tb_chunk){mem, size, 0};\n"; *(const char * *)tb_grow(v_lines) = tb_v407; }
    { const char * tb_v408 = "        c = &tb_chunks[tb_nchunks];\n"; *(const char * *)tb_grow(v_lines) = tb_v408; }
    { const char * tb_v409 = "        tb_nchunks += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v409; }
    { const char * tb_v410 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v410; }
    { const char * tb_v411 = "    void *p = c->mem + c->used;\n"; *(const char * *)tb_grow(v_lines) = tb_v411; }
    { const char * tb_v412 = "    c->used += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v412; }
    { const char * tb_v413 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v413; }
    { const char * tb_v414 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v414; }
    { const char * tb_v415 = "static tb_mark tb_temp_begin(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v415; }
    { const char * tb_v416 = "    tb_temp_depth += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v416; }
    { const char * tb_v417 = "    return (tb_mark){tb_nchunks, tb_nchunks > 0 ? tb_chunks[tb_nchunks - 1].used : 0};\n"; *(const char * *)tb_grow(v_lines) = tb_v417; }
    { const char * tb_v418 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v418; }
    { const char * tb_v419 = "static void tb_temp_end(tb_mark m) {\n"; *(const char * *)tb_grow(v_lines) = tb_v419; }
    { const char * tb_v420 = "    // первый кусок оставляем пустым: следующий temp возьмёт его без malloc\n"; *(const char * *)tb_grow(v_lines) = tb_v420; }
    { const char * tb_v421 = "    while (tb_nchunks > m.n && tb_nchunks > 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v421; }
    { const char * tb_v422 = "        tb_nchunks -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v422; }
    { const char * tb_v423 = "        free(tb_chunks[tb_nchunks].mem);\n"; *(const char * *)tb_grow(v_lines) = tb_v423; }
    { const char * tb_v424 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v424; }
    { const char * tb_v425 = "    if (tb_nchunks > m.n) tb_chunks[0].used = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v425; }
    { const char * tb_v426 = "    else if (tb_nchunks > 0) tb_chunks[tb_nchunks - 1].used = m.used;\n"; *(const char * *)tb_grow(v_lines) = tb_v426; }
    { const char * tb_v427 = "    tb_temp_depth -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v427; }
    { const char * tb_v428 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v428; }
    { const char * tb_v429 = "static void *tb_mem(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v429; }
    { const char * tb_v430 = "    if (tb_temp_depth > 0) return tb_temp_alloc(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v430; }
    { const char * tb_v431 = "    void *p = malloc(n > 0 ? n : 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v431; }
    { const char * tb_v432 = "    if (p == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v432; }
    { const char * tb_v433 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v433; }
    { const char * tb_v434 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v434; }
    { const char * tb_v435 = "// вырасти кусок: временный растёт во временной памяти, обычный — через realloc\n"; *(const char * *)tb_grow(v_lines) = tb_v435; }
    { const char * tb_v436 = "static void *tb_resize(void *p, long long old_n, long long new_n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v436; }
    { const char * tb_v437 = "    if (tb_nchunks > 0 && tb_in_temp(p)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v437; }
    { const char * tb_v438 = "        void *q = tb_temp_alloc(new_n);\n"; *(const char * *)tb_grow(v_lines) = tb_v438; }
    { const char * tb_v439 = "        memcpy(q, p, old_n);\n"; *(const char * *)tb_grow(v_lines) = tb_v439; }
    { const char * tb_v440 = "        return q;\n"; *(const char * *)tb_grow(v_lines) = tb_v440; }
    { const char * tb_v441 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v441; }
    { const char * tb_v442 = "    p = realloc(p, new_n > 0 ? new_n : 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v442; }
    { const char * tb_v443 = "    if (p == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v443; }
    { const char * tb_v444 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v444; }
    { const char * tb_v445 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v445; }
    { const char * tb_v446 = "static tb_list *tb_list_new(long long size) {\n"; *(const char * *)tb_grow(v_lines) = tb_v446; }
    { const char * tb_v447 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v447; }
    { const char * tb_v448 = "    l->len = 0; l->cap = 4; l->size = size; l->data = tb_mem(size * 4);\n"; *(const char * *)tb_grow(v_lines) = tb_v448; }
    { const char * tb_v449 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v449; }
    { const char * tb_v450 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v450; }
    { const char * tb_v451 = "static tb_list *tb_list_from(long long n, long long size, const void *src) {\n"; *(const char * *)tb_grow(v_lines) = tb_v451; }
    { const char * tb_v452 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v452; }
    { const char * tb_v453 = "    l->len = n; l->cap = n; l->size = size; l->data = tb_mem(size * n);\n"; *(const char * *)tb_grow(v_lines) = tb_v453; }
    { const char * tb_v454 = "    memcpy(l->data, src, size * n);\n"; *(const char * *)tb_grow(v_lines) = tb_v454; }
    { const char * tb_v455 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v455; }
    { const char * tb_v456 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v456; }
    { const char * tb_v457 = "// size передаётся как sizeof(T): после встраивания C знает его заранее\n"; *(const char * *)tb_grow(v_lines) = tb_v457; }
    { const char * tb_v458 = "static inline void *tb_at(tb_list *l, long long i, long long size, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v458; }
    { const char * tb_v459 = "    // одно сравнение вместо двух: отрицательный номер как unsigned становится огромным\n"; *(const char * *)tb_grow(v_lines) = tb_v459; }
    { const char * tb_v460 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v460; }
    { const char * tb_v461 = "    return l->data + i * size;\n"; *(const char * *)tb_grow(v_lines) = tb_v461; }
    { const char * tb_v462 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v462; }
    { const char * tb_v463 = "static inline void *tb_grow(tb_list *l) {\n"; *(const char * *)tb_grow(v_lines) = tb_v463; }
    { const char * tb_v464 = "    // cap < 0: это кусок (each piece in data by N) или список, который сейчас режут на куски\n"; *(const char * *)tb_grow(v_lines) = tb_v464; }
    { const char * tb_v465 = "    if (TB_RARE(l->len >= l->cap)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v465; }
    { const char * tb_v466 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v466; }
    { const char * tb_v467 = "        long long old = l->cap * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v467; }
    { const char * tb_v468 = "        l->cap = l->cap * 2 + 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v468; }
    { const char * tb_v469 = "        // realloc часто растит большой кусок на месте, без копирования\n"; *(const char * *)tb_grow(v_lines) = tb_v469; }
    { const char * tb_v470 = "        l->data = tb_resize(l->data, old, l->cap * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v470; }
    { const char * tb_v471 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v471; }
    { const char * tb_v472 = "    l->len += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v472; }
    { const char * tb_v473 = "    return l->data + (l->len - 1) * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v473; }
    { const char * tb_v474 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v474; }
    { const char * tb_v475 = "static void *tb_pop(tb_list *l, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v475; }
    { const char * tb_v476 = "    if (l->len == 0) tb_crash(line, \".pop() у пустого списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v476; }
    { const char * tb_v477 = "    l->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v477; }
    { const char * tb_v478 = "    return l->data + l->len * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v478; }
    { const char * tb_v479 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v479; }
    { const char * tb_v480 = "static void *tb_nn(void *p, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v480; }
    { const char * tb_v481 = "    if (p == 0) tb_crash(line, \"obj пустой: его ещё не создали\");\n"; *(const char * *)tb_grow(v_lines) = tb_v481; }
    { const char * tb_v482 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v482; }
    { const char * tb_v483 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v483; }
    { const char * tb_v484 = "static const char *tb_cat(const char *a, const char *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v484; }
    { const char * tb_v485 = "    size_t la = strlen(a), lb = strlen(b);\n"; *(const char * *)tb_grow(v_lines) = tb_v485; }
    { const char * tb_v486 = "    char *s = tb_mem(la + lb + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v486; }
    { const char * tb_v487 = "    memcpy(s, a, la);\n"; *(const char * *)tb_grow(v_lines) = tb_v487; }
    { const char * tb_v488 = "    memcpy(s + la, b, lb + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v488; }
    { const char * tb_v489 = "    return s;\n"; *(const char * *)tb_grow(v_lines) = tb_v489; }
    { const char * tb_v490 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v490; }
    { const char * tb_v491 = "static const char *tb_itos(long long x) { char *s = tb_mem(32); snprintf(s, 32, \"%lld\", x); return s; }\n"; *(const char * *)tb_grow(v_lines) = tb_v491; }
    { const char * tb_v492 = "static const char *tb_dtos(double x) { char *s = tb_mem(40); snprintf(s, 40, \"%g\", x); return s; }\n"; *(const char * *)tb_grow(v_lines) = tb_v492; }
    { const char * tb_v493 = "static const char *tb_btos(int x) { return x ? \"true\" : \"false\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v493; }
    { const char * tb_v494 = "static const char *tb_chr(long long c) { char *s = tb_mem(2); s[0] = (char)c; s[1] = 0; return s; }\n"; *(const char * *)tb_grow(v_lines) = tb_v494; }
    { const char * tb_v495 = "static const char *tb_char_at(const char *s, long long i, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v495; }
    { const char * tb_v496 = "    if (i < 0 || i >= (long long)strlen(s)) tb_crash(line, \"номер за пределами текста\");\n"; *(const char * *)tb_grow(v_lines) = tb_v496; }
    { const char * tb_v497 = "    return tb_chr((unsigned char)s[i]);\n"; *(const char * *)tb_grow(v_lines) = tb_v497; }
    { const char * tb_v498 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v498; }
    { const char * tb_v499 = "static const char *tb_cut(const char *s, long long from, long long to, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v499; }
    { const char * tb_v500 = "    long long n = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v500; }
    { const char * tb_v501 = "    if (from < 0 || to >= n || from > to + 1) tb_crash(line, \"cut: номера за пределами текста\");\n"; *(const char * *)tb_grow(v_lines) = tb_v501; }
    { const char * tb_v502 = "    char *r = tb_mem(to - from + 2);\n"; *(const char * *)tb_grow(v_lines) = tb_v502; }
    { const char * tb_v503 = "    memcpy(r, s + from, to - from + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v503; }
    { const char * tb_v504 = "    r[to - from + 1] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v504; }
    { const char * tb_v505 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v505; }
    { const char * tb_v506 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v506; }
    { const char * tb_v507 = "static long long tb_code(const char *s, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v507; }
    { const char * tb_v508 = "    if (s[0] == 0) tb_crash(line, \"code: текст пустой\");\n"; *(const char * *)tb_grow(v_lines) = tb_v508; }
    { const char * tb_v509 = "    return (unsigned char)s[0];\n"; *(const char * *)tb_grow(v_lines) = tb_v509; }
    { const char * tb_v510 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v510; }
    { const char * tb_v511 = "static long long tb_div(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"деление на ноль\"); return a / b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v511; }
    { const char * tb_v512 = "static long long tb_mod(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"остаток от деления на ноль\"); return a % b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v512; }
    { const char * tb_v513 = "static long long tb_round(double x) { return (long long)(x < 0 ? x - 0.5 : x + 0.5); }\n"; *(const char * *)tb_grow(v_lines) = tb_v513; }
    { const char * tb_v514 = "static void tb_ps(const char *s) { fputs(s, stdout); }\n"; *(const char * *)tb_grow(v_lines) = tb_v514; }
    { const char * tb_v515 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v515; }
    { const char * tb_v516 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v516; }
    { const char * tb_v517 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v517; }
    { const char * tb_v518 = "// ---------- ошибки fail / try / or / else err ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v518; }
    { const char * tb_v519 = "// Функция, которая сломалась, кладёт текст ошибки в tb_err и сразу выходит.\n"; *(const char * *)tb_grow(v_lines) = tb_v519; }
    { const char * tb_v520 = "// Тот, кто её вызвал, смотрит в tb_err.\n"; *(const char * *)tb_grow(v_lines) = tb_v520; }
    { const char * tb_v521 = "static _Thread_local const char *tb_err = 0;   // у каждого ядра своя\n"; *(const char * *)tb_grow(v_lines) = tb_v521; }
    { const char * tb_v522 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v522; }
    { const char * tb_v523 = "TB_COLD static void tb_die(const char *msg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v523; }
    { const char * tb_v524 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v524; }
    { const char * tb_v525 = "    fprintf(stderr, \"ошибка: %s\\n\", msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v525; }
    { const char * tb_v526 = "    exit(1);\n"; *(const char * *)tb_grow(v_lines) = tb_v526; }
    { const char * tb_v527 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v527; }
    { const char * tb_v528 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v528; }
    { const char * tb_v529 = "// ---------- текст ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v529; }
    { const char * tb_v530 = "static const char *tb_upper(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v530; }
    { const char * tb_v531 = "    size_t n = strlen(s); char *r = tb_mem(n + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v531; }
    { const char * tb_v532 = "    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v532; }
    { const char * tb_v533 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v533; }
    { const char * tb_v534 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v534; }
    { const char * tb_v535 = "static const char *tb_lower(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v535; }
    { const char * tb_v536 = "    size_t n = strlen(s); char *r = tb_mem(n + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v536; }
    { const char * tb_v537 = "    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v537; }
    { const char * tb_v538 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v538; }
    { const char * tb_v539 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v539; }
    { const char * tb_v540 = "static int tb_space(char c) { return c == ' ' || c == '\\t' || c == '\\n' || c == '\\r'; }\n"; *(const char * *)tb_grow(v_lines) = tb_v540; }
    { const char * tb_v541 = "static const char *tb_trim(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v541; }
    { const char * tb_v542 = "    size_t a = 0, b = strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v542; }
    { const char * tb_v543 = "    while (a < b && tb_space(s[a])) a++;\n"; *(const char * *)tb_grow(v_lines) = tb_v543; }
    { const char * tb_v544 = "    while (b > a && tb_space(s[b - 1])) b--;\n"; *(const char * *)tb_grow(v_lines) = tb_v544; }
    { const char * tb_v545 = "    char *r = tb_mem(b - a + 1); memcpy(r, s + a, b - a); r[b - a] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v545; }
    { const char * tb_v546 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v546; }
    { const char * tb_v547 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v547; }
    { const char * tb_v548 = "static long long tb_find(const char *s, const char *part) {\n"; *(const char * *)tb_grow(v_lines) = tb_v548; }
    { const char * tb_v549 = "    const char *p = strstr(s, part);\n"; *(const char * *)tb_grow(v_lines) = tb_v549; }
    { const char * tb_v550 = "    return p ? (long long)(p - s) : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v550; }
    { const char * tb_v551 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v551; }
    { const char * tb_v552 = "static int tb_starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v552; }
    { const char * tb_v553 = "static int tb_ends(const char *s, const char *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v553; }
    { const char * tb_v554 = "    size_t a = strlen(s), b = strlen(p);\n"; *(const char * *)tb_grow(v_lines) = tb_v554; }
    { const char * tb_v555 = "    return b <= a && strcmp(s + a - b, p) == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v555; }
    { const char * tb_v556 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v556; }
    { const char * tb_v557 = "static const char *tb_replace(const char *s, const char *from, const char *to, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v557; }
    { const char * tb_v558 = "    size_t lf = strlen(from), lt = strlen(to), count = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v558; }
    { const char * tb_v559 = "    if (lf == 0) tb_crash(line, \"replace: что заменять — пустой текст\");\n"; *(const char * *)tb_grow(v_lines) = tb_v559; }
    { const char * tb_v560 = "    for (const char *p = strstr(s, from); p; p = strstr(p + lf, from)) count++;\n"; *(const char * *)tb_grow(v_lines) = tb_v560; }
    { const char * tb_v561 = "    char *r = tb_mem(strlen(s) + count * lt + 1), *w = r;\n"; *(const char * *)tb_grow(v_lines) = tb_v561; }
    { const char * tb_v562 = "    const char *p = s, *q;\n"; *(const char * *)tb_grow(v_lines) = tb_v562; }
    { const char * tb_v563 = "    while ((q = strstr(p, from)) != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v563; }
    { const char * tb_v564 = "        memcpy(w, p, q - p); w += q - p;\n"; *(const char * *)tb_grow(v_lines) = tb_v564; }
    { const char * tb_v565 = "        memcpy(w, to, lt); w += lt;\n"; *(const char * *)tb_grow(v_lines) = tb_v565; }
    { const char * tb_v566 = "        p = q + lf;\n"; *(const char * *)tb_grow(v_lines) = tb_v566; }
    { const char * tb_v567 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v567; }
    { const char * tb_v568 = "    strcpy(w, p);\n"; *(const char * *)tb_grow(v_lines) = tb_v568; }
    { const char * tb_v569 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v569; }
    { const char * tb_v570 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v570; }
    { const char * tb_v571 = "static tb_list *tb_split(const char *s, const char *sep, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v571; }
    { const char * tb_v572 = "    size_t ls = strlen(sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v572; }
    { const char * tb_v573 = "    if (ls == 0) tb_crash(line, \"split: разделитель — пустой текст\");\n"; *(const char * *)tb_grow(v_lines) = tb_v573; }
    { const char * tb_v574 = "    tb_list *l = tb_list_new(sizeof(const char *));\n"; *(const char * *)tb_grow(v_lines) = tb_v574; }
    { const char * tb_v575 = "    const char *p = s, *q;\n"; *(const char * *)tb_grow(v_lines) = tb_v575; }
    { const char * tb_v576 = "    while (1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v576; }
    { const char * tb_v577 = "        q = strstr(p, sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v577; }
    { const char * tb_v578 = "        size_t n = q ? (size_t)(q - p) : strlen(p);\n"; *(const char * *)tb_grow(v_lines) = tb_v578; }
    { const char * tb_v579 = "        char *part = tb_mem(n + 1); memcpy(part, p, n); part[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v579; }
    { const char * tb_v580 = "        *(const char **)tb_grow(l) = part;\n"; *(const char * *)tb_grow(v_lines) = tb_v580; }
    { const char * tb_v581 = "        if (!q) break;\n"; *(const char * *)tb_grow(v_lines) = tb_v581; }
    { const char * tb_v582 = "        p = q + ls;\n"; *(const char * *)tb_grow(v_lines) = tb_v582; }
    { const char * tb_v583 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v583; }
    { const char * tb_v584 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v584; }
    { const char * tb_v585 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v585; }
    { const char * tb_v586 = "static const char *tb_join(tb_list *l, const char *sep) {\n"; *(const char * *)tb_grow(v_lines) = tb_v586; }
    { const char * tb_v587 = "    size_t n = 0, ls = strlen(sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v587; }
    { const char * tb_v588 = "    for (long long i = 0; i < l->len; i++) n += strlen(((const char **)l->data)[i]) + ls;\n"; *(const char * *)tb_grow(v_lines) = tb_v588; }
    { const char * tb_v589 = "    char *r = tb_mem(n + 1), *w = r;\n"; *(const char * *)tb_grow(v_lines) = tb_v589; }
    { const char * tb_v590 = "    for (long long i = 0; i < l->len; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v590; }
    { const char * tb_v591 = "        const char *part = ((const char **)l->data)[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v591; }
    { const char * tb_v592 = "        if (i > 0) { memcpy(w, sep, ls); w += ls; }\n"; *(const char * *)tb_grow(v_lines) = tb_v592; }
    { const char * tb_v593 = "        memcpy(w, part, strlen(part)); w += strlen(part);\n"; *(const char * *)tb_grow(v_lines) = tb_v593; }
    { const char * tb_v594 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v594; }
    { const char * tb_v595 = "    *w = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v595; }
    { const char * tb_v596 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v596; }
    { const char * tb_v597 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v597; }
    { const char * tb_v598 = "static long long tb_to_int(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v598; }
    { const char * tb_v599 = "    long long v = 0; int i = 0, minus = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v599; }
    { const char * tb_v600 = "    if (s[0] == '-') { minus = 1; i = 1; }\n"; *(const char * *)tb_grow(v_lines) = tb_v600; }
    { const char * tb_v601 = "    if (s[i] == 0) { tb_err = tb_cat(\"это не число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v601; }
    { const char * tb_v602 = "    for (; s[i]; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v602; }
    { const char * tb_v603 = "        if (s[i] < '0' || s[i] > '9') { tb_err = tb_cat(\"это не число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v603; }
    { const char * tb_v604 = "        v = v * 10 + (s[i] - '0');\n"; *(const char * *)tb_grow(v_lines) = tb_v604; }
    { const char * tb_v605 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v605; }
    { const char * tb_v606 = "    return minus ? -v : v;\n"; *(const char * *)tb_grow(v_lines) = tb_v606; }
    { const char * tb_v607 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v607; }
    { const char * tb_v608 = "static double tb_to_dec(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v608; }
    { const char * tb_v609 = "    char *end = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v609; }
    { const char * tb_v610 = "    double v = strtod(s, &end);\n"; *(const char * *)tb_grow(v_lines) = tb_v610; }
    { const char * tb_v611 = "    if (s[0] == 0 || *end != 0) { tb_err = tb_cat(\"это не дробное число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v611; }
    { const char * tb_v612 = "    return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v612; }
    { const char * tb_v613 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v613; }
    { const char * tb_v614 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v614; }
    { const char * tb_v615 = "// ---------- файлы, ввод, слова запуска ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v615; }
    { const char * tb_v616 = "static const char *tb_read_file(const char *path) {\n"; *(const char * *)tb_grow(v_lines) = tb_v616; }
    { const char * tb_v617 = "    FILE *f = fopen(path, \"rb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v617; }
    { const char * tb_v618 = "    if (f == 0) { tb_err = tb_cat(\"не могу открыть файл: \", path); return \"\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v618; }
    { const char * tb_v619 = "    size_t cap = 4096, n = 0, got;\n"; *(const char * *)tb_grow(v_lines) = tb_v619; }
    { const char * tb_v620 = "    char *buf = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v620; }
    { const char * tb_v621 = "    while ((got = fread(buf + n, 1, cap - n - 1, f)) > 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v621; }
    { const char * tb_v622 = "        n += got;\n"; *(const char * *)tb_grow(v_lines) = tb_v622; }
    { const char * tb_v623 = "        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }\n"; *(const char * *)tb_grow(v_lines) = tb_v623; }
    { const char * tb_v624 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v624; }
    { const char * tb_v625 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v625; }
    { const char * tb_v626 = "    buf[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v626; }
    { const char * tb_v627 = "    return buf;\n"; *(const char * *)tb_grow(v_lines) = tb_v627; }
    { const char * tb_v628 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v628; }
    { const char * tb_v629 = "static void tb_write_file(const char *path, const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v629; }
    { const char * tb_v630 = "    FILE *f = fopen(path, \"wb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v630; }
    { const char * tb_v631 = "    if (f == 0) { tb_err = tb_cat(\"не могу записать файл: \", path); return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v631; }
    { const char * tb_v632 = "    fputs(s, f);\n"; *(const char * *)tb_grow(v_lines) = tb_v632; }
    { const char * tb_v633 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v633; }
    { const char * tb_v634 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v634; }
    { const char * tb_v635 = "static const char *tb_input(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v635; }
    { const char * tb_v636 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v636; }
    { const char * tb_v637 = "    size_t cap = 256, n = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v637; }
    { const char * tb_v638 = "    char *buf = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v638; }
    { const char * tb_v639 = "    int c;\n"; *(const char * *)tb_grow(v_lines) = tb_v639; }
    { const char * tb_v640 = "    while ((c = getchar()) != EOF && c != '\\n') {\n"; *(const char * *)tb_grow(v_lines) = tb_v640; }
    { const char * tb_v641 = "        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }\n"; *(const char * *)tb_grow(v_lines) = tb_v641; }
    { const char * tb_v642 = "        buf[n++] = (char)c;\n"; *(const char * *)tb_grow(v_lines) = tb_v642; }
    { const char * tb_v643 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v643; }
    { const char * tb_v644 = "    if (c == EOF && n == 0) { tb_err = \"ввод кончился\"; return \"\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v644; }
    { const char * tb_v645 = "    if (n > 0 && buf[n - 1] == '\\r') n--;\n"; *(const char * *)tb_grow(v_lines) = tb_v645; }
    { const char * tb_v646 = "    buf[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v646; }
    { const char * tb_v647 = "    return buf;\n"; *(const char * *)tb_grow(v_lines) = tb_v647; }
    { const char * tb_v648 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v648; }
    { const char * tb_v649 = "static tb_list *tb_args;\n"; *(const char * *)tb_grow(v_lines) = tb_v649; }
    { const char * tb_v650 = "static long long tb_run(const char *command) {\n"; *(const char * *)tb_grow(v_lines) = tb_v650; }
    { const char * tb_v651 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v651; }
    { const char * tb_v652 = "    int code = system(command);\n"; *(const char * *)tb_grow(v_lines) = tb_v652; }
    { const char * tb_v653 = "    if (code == -1) return -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v653; }
    { const char * tb_v654 = "    return code == 0 ? 0 : (code > 255 ? code / 256 : code);\n"; *(const char * *)tb_grow(v_lines) = tb_v654; }
    { const char * tb_v655 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v655; }
    { const char * tb_v656 = "static void tb_exit(long long code) { fflush(stdout); exit((int)code); }\n"; *(const char * *)tb_grow(v_lines) = tb_v656; }
    { const char * tb_v657 = "static int tb_file_exists(const char *path) {\n"; *(const char * *)tb_grow(v_lines) = tb_v657; }
    { const char * tb_v658 = "    FILE *f = fopen(path, \"rb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v658; }
    { const char * tb_v659 = "    if (f == 0) return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v659; }
    { const char * tb_v660 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v660; }
    { const char * tb_v661 = "    return 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v661; }
    { const char * tb_v662 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v662; }
    { const char * tb_v663 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v663; }
    { const char * tb_v664 = "// ---------- числа ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v664; }
    { const char * tb_v665 = "static long long tb_absi(long long x) { return x < 0 ? -x : x; }\n"; *(const char * *)tb_grow(v_lines) = tb_v665; }
    { const char * tb_v666 = "static long long tb_mini(long long a, long long b) { return a < b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v666; }
    { const char * tb_v667 = "static long long tb_maxi(long long a, long long b) { return a > b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v667; }
    { const char * tb_v668 = "static double tb_mind(double a, double b) { return a < b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v668; }
    { const char * tb_v669 = "static double tb_maxd(double a, double b) { return a > b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v669; }
    { const char * tb_v670 = "static double tb_sqrt(double x, int line) { if (x < 0) tb_crash(line, \"sqrt: корень из отрицательного числа\"); return sqrt(x); }\n"; *(const char * *)tb_grow(v_lines) = tb_v670; }
    { const char * tb_v671 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v671; }
    { const char * tb_v672 = "// ---------- sort ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v672; }
    { const char * tb_v673 = "static int tb_cmp_int(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return (x > y) - (x < y); }\n"; *(const char * *)tb_grow(v_lines) = tb_v673; }
    { const char * tb_v674 = "static int tb_cmp_dec(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }\n"; *(const char * *)tb_grow(v_lines) = tb_v674; }
    { const char * tb_v675 = "static int tb_cmp_text(const void *a, const void *b) { return strcmp(*(const char * const *)a, *(const char * const *)b); }\n"; *(const char * *)tb_grow(v_lines) = tb_v675; }
    { const char * tb_v676 = "static tb_list *tb_copy(tb_list *l) { return tb_list_from(l->len, l->size, l->data); }\n"; *(const char * *)tb_grow(v_lines) = tb_v676; }
    { const char * tb_v677 = "static void tb_sort_list(tb_list *l, int (*cmp)(const void *, const void *)) { qsort(l->data, l->len, l->size, cmp); }\n"; *(const char * *)tb_grow(v_lines) = tb_v677; }
    { const char * tb_v678 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v678; }
    { const char * tb_v679 = "// ---------- dict: ключи и значения лежат в двух списках по порядку добавления,\n"; *(const char * *)tb_grow(v_lines) = tb_v679; }
    { const char * tb_v680 = "// а таблица slots быстро находит номер по ключу (номер + 1, 0 — пусто) ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v680; }
    { const char * tb_v681 = "typedef struct { tb_list *keys; tb_list *vals; long long *slots; long long nslots; int text; } tb_dict;\n"; *(const char * *)tb_grow(v_lines) = tb_v681; }
    { const char * tb_v682 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v682; }
    { const char * tb_v683 = "static unsigned long long tb_hash(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v683; }
    { const char * tb_v684 = "    unsigned long long h = 1469598103934665603ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v684; }
    { const char * tb_v685 = "    if (d->text) {\n"; *(const char * *)tb_grow(v_lines) = tb_v685; }
    { const char * tb_v686 = "        for (const unsigned char *p = *(const unsigned char * const *)k; *p; p++) { h ^= *p; h *= 1099511628211ULL; }\n"; *(const char * *)tb_grow(v_lines) = tb_v686; }
    { const char * tb_v687 = "    } else {\n"; *(const char * *)tb_grow(v_lines) = tb_v687; }
    { const char * tb_v688 = "        h ^= (unsigned long long)*(const long long *)k;\n"; *(const char * *)tb_grow(v_lines) = tb_v688; }
    { const char * tb_v689 = "        h *= 0x9E3779B97F4A7C15ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v689; }
    { const char * tb_v690 = "        h ^= h >> 32;\n"; *(const char * *)tb_grow(v_lines) = tb_v690; }
    { const char * tb_v691 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v691; }
    { const char * tb_v692 = "    return h;\n"; *(const char * *)tb_grow(v_lines) = tb_v692; }
    { const char * tb_v693 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v693; }
    { const char * tb_v694 = "static int tb_keq(tb_dict *d, const void *a, const void *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v694; }
    { const char * tb_v695 = "    if (d->text) return strcmp(*(const char * const *)a, *(const char * const *)b) == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v695; }
    { const char * tb_v696 = "    return *(const long long *)a == *(const long long *)b;\n"; *(const char * *)tb_grow(v_lines) = tb_v696; }
    { const char * tb_v697 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v697; }
    { const char * tb_v698 = "static long long tb_dfind(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v698; }
    { const char * tb_v699 = "    unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v699; }
    { const char * tb_v700 = "    while (d->slots[i] != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v700; }
    { const char * tb_v701 = "        long long at = d->slots[i] - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v701; }
    { const char * tb_v702 = "        if (tb_keq(d, d->keys->data + at * d->keys->size, k)) return at;\n"; *(const char * *)tb_grow(v_lines) = tb_v702; }
    { const char * tb_v703 = "        i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v703; }
    { const char * tb_v704 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v704; }
    { const char * tb_v705 = "    return -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v705; }
    { const char * tb_v706 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v706; }
    { const char * tb_v707 = "static void tb_dreindex(tb_dict *d, long long nslots) {\n"; *(const char * *)tb_grow(v_lines) = tb_v707; }
    { const char * tb_v708 = "    // новая таблица там же, где старая: временная — во временной памяти\n"; *(const char * *)tb_grow(v_lines) = tb_v708; }
    { const char * tb_v709 = "    int in_temp = d->slots != 0 ? tb_nchunks > 0 && tb_in_temp(d->slots) : tb_temp_depth > 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v709; }
    { const char * tb_v710 = "    if (!in_temp) free(d->slots);\n"; *(const char * *)tb_grow(v_lines) = tb_v710; }
    { const char * tb_v711 = "    d->nslots = nslots;\n"; *(const char * *)tb_grow(v_lines) = tb_v711; }
    { const char * tb_v712 = "    d->slots = in_temp ? tb_temp_alloc(nslots * sizeof(long long)) : malloc(nslots * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v712; }
    { const char * tb_v713 = "    if (!d->slots) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v713; }
    { const char * tb_v714 = "    memset(d->slots, 0, nslots * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v714; }
    { const char * tb_v715 = "    unsigned long long mask = nslots - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v715; }
    { const char * tb_v716 = "    for (long long at = 0; at < d->keys->len; at++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v716; }
    { const char * tb_v717 = "        unsigned long long i = tb_hash(d, d->keys->data + at * d->keys->size) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v717; }
    { const char * tb_v718 = "        while (d->slots[i] != 0) i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v718; }
    { const char * tb_v719 = "        d->slots[i] = at + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v719; }
    { const char * tb_v720 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v720; }
    { const char * tb_v721 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v721; }
    { const char * tb_v722 = "static tb_dict *tb_dict_new(int text, long long vsize) {\n"; *(const char * *)tb_grow(v_lines) = tb_v722; }
    { const char * tb_v723 = "    tb_dict *d = tb_mem(sizeof(tb_dict));\n"; *(const char * *)tb_grow(v_lines) = tb_v723; }
    { const char * tb_v724 = "    d->text = text;\n"; *(const char * *)tb_grow(v_lines) = tb_v724; }
    { const char * tb_v725 = "    d->keys = tb_list_new(text ? sizeof(const char *) : sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v725; }
    { const char * tb_v726 = "    d->vals = tb_list_new(vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v726; }
    { const char * tb_v727 = "    d->slots = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v727; }
    { const char * tb_v728 = "    tb_dreindex(d, 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v728; }
    { const char * tb_v729 = "    return d;\n"; *(const char * *)tb_grow(v_lines) = tb_v729; }
    { const char * tb_v730 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v730; }
    { const char * tb_v731 = "// место под значение ключа k; если ключа нет — добавить со значением *def\n"; *(const char * *)tb_grow(v_lines) = tb_v731; }
    { const char * tb_v732 = "static void *tb_dput(tb_dict *d, const void *k, const void *def) {\n"; *(const char * *)tb_grow(v_lines) = tb_v732; }
    { const char * tb_v733 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v733; }
    { const char * tb_v734 = "    if (at >= 0) return d->vals->data + at * d->vals->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v734; }
    { const char * tb_v735 = "    memcpy(tb_grow(d->keys), k, d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v735; }
    { const char * tb_v736 = "    void *v = tb_grow(d->vals);\n"; *(const char * *)tb_grow(v_lines) = tb_v736; }
    { const char * tb_v737 = "    if (def) memcpy(v, def, d->vals->size); else memset(v, 0, d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v737; }
    { const char * tb_v738 = "    if (d->keys->len * 2 > d->nslots) tb_dreindex(d, d->nslots * 2);\n"; *(const char * *)tb_grow(v_lines) = tb_v738; }
    { const char * tb_v739 = "    else {\n"; *(const char * *)tb_grow(v_lines) = tb_v739; }
    { const char * tb_v740 = "        unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v740; }
    { const char * tb_v741 = "        while (d->slots[i] != 0) i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v741; }
    { const char * tb_v742 = "        d->slots[i] = d->keys->len;\n"; *(const char * *)tb_grow(v_lines) = tb_v742; }
    { const char * tb_v743 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v743; }
    { const char * tb_v744 = "    return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v744; }
    { const char * tb_v745 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v745; }
    { const char * tb_v746 = "static void *tb_dget(tb_dict *d, const void *k, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v746; }
    { const char * tb_v747 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v747; }
    { const char * tb_v748 = "    if (at < 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v748; }
    { const char * tb_v749 = "        if (d->text) tb_crash(line, tb_cat(\"в dict нет ключа: \", *(const char * const *)k));\n"; *(const char * *)tb_grow(v_lines) = tb_v749; }
    { const char * tb_v750 = "        tb_crash(line, tb_cat(\"в dict нет ключа: \", tb_itos(*(const long long *)k)));\n"; *(const char * *)tb_grow(v_lines) = tb_v750; }
    { const char * tb_v751 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v751; }
    { const char * tb_v752 = "    return d->vals->data + at * d->vals->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v752; }
    { const char * tb_v753 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v753; }
    { const char * tb_v754 = "// together: шаг 1 — место ключа в таблице, просим память принести его заранее\n"; *(const char * *)tb_grow(v_lines) = tb_v754; }
    { const char * tb_v755 = "static inline unsigned long long tb_dpre1(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v755; }
    { const char * tb_v756 = "    unsigned long long i = tb_hash(d, k) & (d->nslots - 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v756; }
    { const char * tb_v757 = "    __builtin_prefetch(&d->slots[i]);\n"; *(const char * *)tb_grow(v_lines) = tb_v757; }
    { const char * tb_v758 = "    return i;\n"; *(const char * *)tb_grow(v_lines) = tb_v758; }
    { const char * tb_v759 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v759; }
    { const char * tb_v760 = "// шаг 2 — место приехало: просим принести ключ и значение\n"; *(const char * *)tb_grow(v_lines) = tb_v760; }
    { const char * tb_v761 = "static inline void tb_dpre2(tb_dict *d, unsigned long long i) {\n"; *(const char * *)tb_grow(v_lines) = tb_v761; }
    { const char * tb_v762 = "    long long s = d->slots[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v762; }
    { const char * tb_v763 = "    if (s != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v763; }
    { const char * tb_v764 = "        __builtin_prefetch(d->keys->data + (s - 1) * d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v764; }
    { const char * tb_v765 = "        __builtin_prefetch(d->vals->data + (s - 1) * d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v765; }
    { const char * tb_v766 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v766; }
    { const char * tb_v767 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v767; }
    { const char * tb_v768 = "// элемент списка заранее (prefetch за пределами списка ничего не ломает)\n"; *(const char * *)tb_grow(v_lines) = tb_v768; }
    { const char * tb_v769 = "static inline void tb_lpre(tb_list *l, long long i) {\n"; *(const char * *)tb_grow(v_lines) = tb_v769; }
    { const char * tb_v770 = "    __builtin_prefetch(l->data + i * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v770; }
    { const char * tb_v771 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v771; }
    { const char * tb_v772 = "// ---------- background: работа в своём потоке ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v772; }
    { const char * tb_v773 = "typedef struct { pthread_t th; int done; void *res; } tb_task;\n"; *(const char * *)tb_grow(v_lines) = tb_v773; }
    { const char * tb_v774 = "static void tb_task_start(tb_task *t, void *(*run)(void *), void *arg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v774; }
    { const char * tb_v775 = "    t->done = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v775; }
    { const char * tb_v776 = "    if (pthread_create(&t->th, 0, run, arg) != 0) tb_crash(0, \"не получилось запустить background\");\n"; *(const char * *)tb_grow(v_lines) = tb_v776; }
    { const char * tb_v777 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v777; }
    { const char * tb_v778 = "// a.wait: дождаться потока (один раз) и отдать адрес ответа\n"; *(const char * *)tb_grow(v_lines) = tb_v778; }
    { const char * tb_v779 = "static void *tb_wait(tb_task *t) {\n"; *(const char * *)tb_grow(v_lines) = tb_v779; }
    { const char * tb_v780 = "    if (!t->done) { pthread_join(t->th, 0); t->done = 1; }\n"; *(const char * *)tb_grow(v_lines) = tb_v780; }
    { const char * tb_v781 = "    return t->res;\n"; *(const char * *)tb_grow(v_lines) = tb_v781; }
    { const char * tb_v782 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v782; }
    { const char * tb_v783 = "static int tb_dhas(tb_dict *d, const void *k) { return tb_dfind(d, k) >= 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v783; }
    { const char * tb_v784 = "static void tb_dremove(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v784; }
    { const char * tb_v785 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v785; }
    { const char * tb_v786 = "    if (at < 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v786; }
    { const char * tb_v787 = "    long long last = d->keys->len - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v787; }
    { const char * tb_v788 = "    // на место удалённого ставим последний: порядок меняется только у него\n"; *(const char * *)tb_grow(v_lines) = tb_v788; }
    { const char * tb_v789 = "    memcpy(d->keys->data + at * d->keys->size, d->keys->data + last * d->keys->size, d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v789; }
    { const char * tb_v790 = "    memcpy(d->vals->data + at * d->vals->size, d->vals->data + last * d->vals->size, d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v790; }
    { const char * tb_v791 = "    d->keys->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v791; }
    { const char * tb_v792 = "    d->vals->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v792; }
    { const char * tb_v793 = "    tb_dreindex(d, d->nslots);\n"; *(const char * *)tb_grow(v_lines) = tb_v793; }
    { const char * tb_v794 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v794; }
    { const char * tb_v795 = "static tb_dict *tb_dict_from(int text, long long vsize, long long n, const void *keys, const void *vals) {\n"; *(const char * *)tb_grow(v_lines) = tb_v795; }
    { const char * tb_v796 = "    tb_dict *d = tb_dict_new(text, vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v796; }
    { const char * tb_v797 = "    long long ksize = d->keys->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v797; }
    { const char * tb_v798 = "    for (long long i = 0; i < n; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v798; }
    { const char * tb_v799 = "        memcpy(tb_dput(d, (const char *)keys + i * ksize, 0), (const char *)vals + i * vsize, vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v799; }
    { const char * tb_v800 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v800; }
    { const char * tb_v801 = "    return d;\n"; *(const char * *)tb_grow(v_lines) = tb_v801; }
    { const char * tb_v802 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v802; }
    { const char * tb_v803 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v803; }
    { const char * tb_v804 = "// ---------- on all cores: цикл на всех ядрах ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v804; }
    { const char * tb_v805 = "// Работа делится на куски; каждое ядро берёт следующий свободный кусок,\n"; *(const char * *)tb_grow(v_lines) = tb_v805; }
    { const char * tb_v806 = "// пока они не кончатся (так ядра не простаивают, если куски разные по тяжести).\n"; *(const char * *)tb_grow(v_lines) = tb_v806; }
    { const char * tb_v807 = "// shared: каждое ядро копит своё на «листке», потом один раз добавляет под замком.\n"; *(const char * *)tb_grow(v_lines) = tb_v807; }
    { const char * tb_v808 = "static pthread_mutex_t tb_par_lock = PTHREAD_MUTEX_INITIALIZER;\n"; *(const char * *)tb_grow(v_lines) = tb_v808; }
    { const char * tb_v809 = "static int tb_cores(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v809; }
    { const char * tb_v810 = "    const char *want = getenv(\"TB_CORES\");   // TB_CORES=1 — проверить на одном ядре\n"; *(const char * *)tb_grow(v_lines) = tb_v810; }
    { const char * tb_v811 = "    long n = want != 0 ? atol(want) : sysconf(_SC_NPROCESSORS_ONLN);\n"; *(const char * *)tb_grow(v_lines) = tb_v811; }
    { const char * tb_v812 = "    return n < 1 ? 1 : (n > 256 ? 256 : (int)n);\n"; *(const char * *)tb_grow(v_lines) = tb_v812; }
    { const char * tb_v813 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v813; }
    { const char * tb_v814 = "static void tb_par_run(void *(*work)(void *), void *job, long long total) {\n"; *(const char * *)tb_grow(v_lines) = tb_v814; }
    { const char * tb_v815 = "    int n = tb_cores();\n"; *(const char * *)tb_grow(v_lines) = tb_v815; }
    { const char * tb_v816 = "    if (n > total) n = (int)total;\n"; *(const char * *)tb_grow(v_lines) = tb_v816; }
    { const char * tb_v817 = "    if (n < 1) n = 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v817; }
    { const char * tb_v818 = "    pthread_t t[256];\n"; *(const char * *)tb_grow(v_lines) = tb_v818; }
    { const char * tb_v819 = "    for (int i = 1; i < n; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v819; }
    { const char * tb_v820 = "        if (pthread_create(&t[i], 0, work, job) != 0) tb_crash(0, \"не получилось запустить ядро\");\n"; *(const char * *)tb_grow(v_lines) = tb_v820; }
    { const char * tb_v821 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v821; }
    { const char * tb_v822 = "    work(job);\n"; *(const char * *)tb_grow(v_lines) = tb_v822; }
    { const char * tb_v823 = "    for (int i = 1; i < n; i++) pthread_join(t[i], 0);\n"; *(const char * *)tb_grow(v_lines) = tb_v823; }
    { const char * tb_v824 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v824; }
    { const char * tb_v825 = "static long long tb_par_step(long long total) {\n"; *(const char * *)tb_grow(v_lines) = tb_v825; }
    { const char * tb_v826 = "    long long s = total / ((long long)tb_cores() * 16);\n"; *(const char * *)tb_grow(v_lines) = tb_v826; }
    { const char * tb_v827 = "    return s < 1 ? 1 : s;\n"; *(const char * *)tb_grow(v_lines) = tb_v827; }
    { const char * tb_v828 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v828; }
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
