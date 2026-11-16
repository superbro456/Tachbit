// Сделано компилятором Tachbit 0.7. Не правь руками: правь файл .tb
// Библиотека, которую Tachbit кладёт в начало каждой программы на C.
// Это обычный C. Компилятор на Tachbit встраивает этот текст через runtime_text.tb,
// который делает tools/embed_runtime.py. Поменял этот файл — запусти:
//     python3 tools/embed_runtime.py
#ifdef TB_BARE
// ---------- режим без ОС (tb bare): своя маленькая библиотека вместо libc ----------
// Программа сама себе ОС: загружается через multiboot (QEMU, GRUB), пишет на экран
// (текстовый режим VGA) и в последовательный порт COM1, память берёт из своего запаса.
// Ядер, файлов и ввода здесь нет: on all cores и background работают на одном ядре.
#include <stddef.h>
#include <stdarg.h>
#define _Thread_local
#define EOF (-1)
typedef struct tb_file FILE;
#define stdout ((FILE *)1)
#define stderr ((FILE *)2)
typedef int pthread_t;
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define pthread_mutex_lock(m) ((void)(m))
#define pthread_mutex_unlock(m) ((void)(m))
#define _SC_NPROCESSORS_ONLN 0

static inline void tb_outb(unsigned short port, unsigned char v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline unsigned char tb_inb(unsigned short port) { unsigned char v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

// экран 80×25: байт буквы и байт цвета; русские буквы VGA не умеет — на экране «?», в порту как есть
static volatile unsigned short *const tb_vga = (volatile unsigned short *)0xB8000;
static int tb_vrow = 0, tb_vcol = 0;
static unsigned char tb_vcolor = 0x0A;   // светло-зелёный на чёрном
static void tb_vga_put(unsigned char ch) {
    if (ch == '\n') { tb_vcol = 0; tb_vrow++; }
    else {
        tb_vga[tb_vrow * 80 + tb_vcol] = (unsigned short)(tb_vcolor << 8 | ch);
        if (++tb_vcol == 80) { tb_vcol = 0; tb_vrow++; }
    }
    if (tb_vrow == 25) {
        for (int i = 0; i < 24 * 80; i++) tb_vga[i] = tb_vga[i + 80];
        for (int i = 24 * 80; i < 25 * 80; i++) tb_vga[i] = (unsigned short)(tb_vcolor << 8 | ' ');
        tb_vrow = 24;
    }
}
static void tb_putc(char c) {
    unsigned char ch = (unsigned char)c;
    while ((tb_inb(0x3F8 + 5) & 0x20) == 0) {}
    tb_outb(0x3F8, ch);
    if (ch < 0x80) tb_vga_put(ch);
    else if (ch >= 0xC0) tb_vga_put('?');   // первый байт русской буквы (UTF-8) → «?», остальные пропускаем
}
static int fputs(const char *s, FILE *f) { (void)f; while (*s) tb_putc(*s++); return 0; }
static int fflush(FILE *f) { (void)f; return 0; }
static int isatty(int fd) { (void)fd; return 0; }
static char *getenv(const char *n) { (void)n; return 0; }

// выход: QEMU с -device isa-debug-exit закрывается, без него процессор просто спит
__attribute__((noreturn)) static void exit(int code) {
    tb_outb(0xF4, (unsigned char)code);
    for (;;) __asm__ volatile("cli; hlt");
}

// память: запас 64 МБ. Куски по размерам 16, 32, 64, ... байт; отданный кусок
// кладётся в корзину своего размера и выдаётся снова. Последний кусок отдаётся сразу.
#define TB_HEAP (64 << 20)
static unsigned char tb_heap[TB_HEAP] __attribute__((aligned(16)));
static size_t tb_heap_top = 0;
static void *tb_bins[32];
static int tb_cls(size_t n) { int k = 4; while (k < 31 && ((size_t)1 << k) < n) k++; return k; }
static void *malloc(size_t n) {
    int k = tb_cls(n);
    if (tb_bins[k]) { void *p = tb_bins[k]; tb_bins[k] = *(void **)p; return p; }
    size_t need = ((size_t)1 << k) + 16;
    if (tb_heap_top + need > TB_HEAP) return 0;
    unsigned char *h = tb_heap + tb_heap_top;
    *(size_t *)h = (size_t)k;
    tb_heap_top += need;
    return h + 16;
}
static void free(void *p) {
    if (p == 0) return;
    unsigned char *h = (unsigned char *)p - 16;
    int k = (int)*(size_t *)h;
    if (h + ((size_t)1 << k) + 16 == tb_heap + tb_heap_top) { tb_heap_top = (size_t)(h - tb_heap); return; }
    *(void **)p = tb_bins[k];
    tb_bins[k] = p;
}
void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int v, size_t n) {
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
    return r;
}
int memcmp(const void *x, const void *y, size_t n) {
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
static void *realloc(void *p, size_t n) {
    if (p == 0) return malloc(n);
    unsigned char *h = (unsigned char *)p - 16;
    int k = (int)*(size_t *)h;
    size_t cap = (size_t)1 << k;
    if (n <= cap) return p;
    // последний кусок растёт на месте
    if (h + cap + 16 == tb_heap + tb_heap_top) {
        int k2 = tb_cls(n);
        size_t need = ((size_t)1 << k2) + 16;
        if ((size_t)(h - tb_heap) + need <= TB_HEAP) {
            *(size_t *)h = (size_t)k2;
            tb_heap_top = (size_t)(h - tb_heap) + need;
            return p;
        }
    }
    void *q = malloc(n);
    if (q) { memcpy(q, p, cap); free(p); }
    return q;
}
// одно ядро и нет прерываний: «атомарные» действия — обычные
unsigned long long __atomic_fetch_add_8(volatile void *p, unsigned long long v, int order) {
    (void)order;
    unsigned long long old = *(volatile unsigned long long *)p;
    *(volatile unsigned long long *)p = old + v;
    return old;
}
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b; }
    return 0;
}
static char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
static char *strstr(const char *h, const char *n) {
    size_t ln = strlen(n);
    if (ln == 0) return (char *)h;
    for (; *h; h++) if (*h == *n && strncmp(h, n, ln) == 0) return (char *)h;
    return 0;
}
static long atol(const char *s) { long r = 0; while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0'); return r; }
static double sqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
static double fabs(double x) { return x < 0 ? -x : x; }
static double strtod(const char *s, char **end) {
    double r = 0, sign = 1, scale = 1;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { r = r * 10 + (*s++ - '0'); scale *= 10; } }
    if (end) *end = (char *)s;
    return sign * r / scale;
}
// деление 64-битных чисел на 32-битном процессоре: этих функций без libgcc нет
unsigned long long __udivmoddi4(unsigned long long a, unsigned long long b, unsigned long long *rem) {
    unsigned long long q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) { r -= b; q |= 1ULL << i; }
    }
    if (rem) *rem = r;
    return q;
}
unsigned long long __udivdi3(unsigned long long a, unsigned long long b) { return __udivmoddi4(a, b, 0); }
unsigned long long __umoddi3(unsigned long long a, unsigned long long b) { unsigned long long r; __udivmoddi4(a, b, &r); return r; }
long long __divdi3(long long a, long long b) {
    int neg = (a < 0) != (b < 0);
    unsigned long long q = __udivmoddi4(a < 0 ? 0 - (unsigned long long)a : (unsigned long long)a, b < 0 ? 0 - (unsigned long long)b : (unsigned long long)b, 0);
    return neg ? -(long long)q : (long long)q;
}
long long __moddi3(long long a, long long b) {
    unsigned long long r;
    __udivmoddi4(a < 0 ? 0 - (unsigned long long)a : (unsigned long long)a, b < 0 ? 0 - (unsigned long long)b : (unsigned long long)b, &r);
    return a < 0 ? -(long long)r : (long long)r;
}
// маленький printf: %lld %ld %d %s %c %g (дробь — до 6 знаков после точки)
static int tb_vfmt(char *out, size_t cap, const char *f, va_list ap) {
    size_t n = 0;
#define TB_O(ch) do { if (n + 1 < cap) out[n] = (ch); n++; } while (0)
    for (; *f; f++) {
        if (*f != '%') { TB_O(*f); continue; }
        f++;
        int ll = 0;
        while (*f == 'l') { ll++; f++; }
        if (*f == 's') { const char *s = va_arg(ap, const char *); while (*s) TB_O(*s++); }
        else if (*f == 'c') TB_O((char)va_arg(ap, int));
        else if (*f == 'd') {
            long long v = ll >= 2 ? va_arg(ap, long long) : ll == 1 ? va_arg(ap, long) : va_arg(ap, int);
            char d[24]; int k = 0;
            unsigned long long u = v < 0 ? 0 - (unsigned long long)v : (unsigned long long)v;
            do { d[k++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (v < 0) TB_O('-');
            while (k) TB_O(d[--k]);
        } else if (*f == 'g') {
            double x = va_arg(ap, double);
            if (x < 0) { TB_O('-'); x = -x; }
            unsigned long long ip = (unsigned long long)x;
            double fr = x - (double)ip;
            unsigned long long fp = (unsigned long long)(fr * 1000000 + 0.5);
            if (fp >= 1000000) { ip++; fp -= 1000000; }
            char d[24]; int k = 0;
            do { d[k++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
            while (k) TB_O(d[--k]);
            if (fp) {
                TB_O('.');
                char fd[6]; for (int i = 5; i >= 0; i--) { fd[i] = (char)('0' + fp % 10); fp /= 10; }
                int last = 5; while (last > 0 && fd[last] == '0') last--;
                for (int i = 0; i <= last; i++) TB_O(fd[i]);
            }
        } else TB_O(*f);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
#undef TB_O
    return (int)n;
}
static int snprintf(char *out, size_t cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = tb_vfmt(out, cap, f, ap); va_end(ap); return n; }
static int printf(const char *f, ...) { char b[512]; va_list ap; va_start(ap, f); int n = tb_vfmt(b, sizeof b, f, ap); va_end(ap); fputs(b, stdout); return n; }
static int fprintf(FILE *o, const char *f, ...) { char b[512]; va_list ap; va_start(ap, f); int n = tb_vfmt(b, sizeof b, f, ap); va_end(ap); fputs(b, o); return n; }
// сортировка вставками (списки в ядре маленькие)
static void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    unsigned char *a = base, tmp[64];
    if (size > sizeof tmp) return;
    for (size_t i = 1; i < n; i++) {
        memcpy(tmp, a + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(a + (j - 1) * size, tmp) > 0) { memcpy(a + j * size, a + (j - 1) * size, size); j--; }
        memcpy(a + j * size, tmp, size);
    }
}
// файлов, ввода и команд нет
static FILE *fopen(const char *p, const char *m) { (void)p; (void)m; return 0; }
static int fclose(FILE *f) { (void)f; return 0; }
static size_t fread(void *b, size_t s, size_t n, FILE *f) { (void)b; (void)s; (void)n; (void)f; return 0; }
static int getchar(void) { return EOF; }
static int system(const char *c) { (void)c; return -1; }
// одно ядро: «потоки» выполняются сразу, по очереди
static long sysconf(int n) { (void)n; return 1; }
static int pthread_create(pthread_t *t, void *a, void *(*run)(void *), void *arg) { (void)t; (void)a; run(arg); return 0; }
static int pthread_join(pthread_t t, void **r) { (void)t; (void)r; return 0; }

// загрузка: заголовок multiboot (его ищут QEMU и GRUB), свой стек, потом tb_kstart
__attribute__((section(".multiboot"), used, aligned(4)))
static const unsigned int tb_multiboot[3] = {0x1BADB002u, 0u, (unsigned int)-0x1BADB002};
static unsigned char tb_stack[1 << 20] __attribute__((aligned(16), used));
void tb_kmain(void);
__attribute__((used)) void tb_kstart(void) {
    __asm__ volatile("fninit");
    // COM1: 115200 бод, 8 бит
    tb_outb(0x3F8 + 1, 0x00); tb_outb(0x3F8 + 3, 0x80); tb_outb(0x3F8 + 0, 0x01);
    tb_outb(0x3F8 + 1, 0x00); tb_outb(0x3F8 + 3, 0x03); tb_outb(0x3F8 + 2, 0xC7);
    for (int i = 0; i < 80 * 25; i++) tb_vga[i] = (unsigned short)(tb_vcolor << 8 | ' ');
    tb_outb(0x3D4, 0x0A); tb_outb(0x3D5, 0x20);  // мигающий курсор не нужен
    tb_kmain();
    exit(0);
}
__asm__(".text\n.global _start\n_start:\n  mov $tb_stack + 1048576, %esp\n  call tb_kstart\n1: cli\n  hlt\n  jmp 1b\n");
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#endif

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
    _Bool q_bare;
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
static tb_o_Comp *tb_new_Comp(const char * a0, tb_list * a1, long long a2, tb_list * a3, tb_list * a4, tb_list * a5, tb_list * a6, tb_list * a7, const char * a8, tb_list * a9, long long a10, long long a11, long long a12, long long a13, tb_list * a14, long long a15, tb_o_Node * a16, long long a17, long long a18, _Bool a19, _Bool a20, tb_list * a21, long long a22, tb_o_Node * a23, tb_list * a24, tb_list * a25, tb_list * a26, long long a27, tb_list * a28, tb_list * a29, tb_list * a30, long long a31, tb_list * a32, tb_list * a33, tb_list * a34, tb_list * a35, tb_list * a36, tb_list * a37, tb_list * a38, tb_list * a39, const char * a40, _Bool a41, _Bool a42) {
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
    o->q_bare = a42;
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
static long long f_main__build_bare(tb_o_Comp * v_c, const char * v_path, const char * v_exe);
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
    tb_ps(", bare: ");
    tb_show_3(x->q_bare);
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

static long long f_main__build_bare(tb_o_Comp * v_c, const char * v_path, const char * v_exe) {
    long long tb_rl_script = -1, tb_rc_script = 0; (void)tb_rl_script; (void)tb_rc_script;
    long long tb_rl_cc = -1, tb_rc_cc = 0; (void)tb_rl_cc; (void)tb_rc_cc;
    long long tb_rl_ld_cmd = -1, tb_rc_ld_cmd = 0; (void)tb_rl_ld_cmd; (void)tb_rc_ld_cmd;
    { _Bool tb_v56 = 1; _Bool *tb_p56 = &(((tb_o_Comp *)tb_nn(v_c, 144))->q_bare); *tb_p56 = tb_v56; }
    const char * v_c_path = ({ const char *tb_c57_0 = v_exe; long long tb_c57_0n = (long long)strlen(tb_c57_0); const char *tb_c57_1 = ".c"; long long tb_c57_1n = sizeof(".c") - 1; char *tb_c57r = tb_mem(tb_c57_0n + tb_c57_1n + 1), *tb_c57rw = tb_c57r; memcpy(tb_c57rw, tb_c57_0, tb_c57_0n); tb_c57rw += tb_c57_0n; memcpy(tb_c57rw, tb_c57_1, tb_c57_1n); tb_c57rw += tb_c57_1n; *tb_c57rw = 0; (const char *)tb_c57r; });
    const char * v_ld_path = ({ const char *tb_c58_0 = v_exe; long long tb_c58_0n = (long long)strlen(tb_c58_0); const char *tb_c58_1 = ".ld"; long long tb_c58_1n = sizeof(".ld") - 1; char *tb_c58r = tb_mem(tb_c58_0n + tb_c58_1n + 1), *tb_c58rw = tb_c58r; memcpy(tb_c58rw, tb_c58_0, tb_c58_0n); tb_c58rw += tb_c58_0n; memcpy(tb_c58rw, tb_c58_1, tb_c58_1n); tb_c58rw += tb_c58_1n; *tb_c58rw = 0; (const char *)tb_c58r; });
    const char * v_elf = ({ const char *tb_c59_0 = v_exe; long long tb_c59_0n = (long long)strlen(tb_c59_0); const char *tb_c59_1 = ".elf"; long long tb_c59_1n = sizeof(".elf") - 1; char *tb_c59r = tb_mem(tb_c59_0n + tb_c59_1n + 1), *tb_c59rw = tb_c59r; memcpy(tb_c59rw, tb_c59_0, tb_c59_0n); tb_c59rw += tb_c59_0n; memcpy(tb_c59rw, tb_c59_1, tb_c59_1n); tb_c59rw += tb_c59_1n; *tb_c59rw = 0; (const char *)tb_c59r; });
    tb_o_Node * v_program = f_main__load_program(v_c, v_path);
    f_check__check(v_c, v_program);
    f_emit__emit_c(v_c, v_program);
    tb_write_file(v_c_path, tb_join(((tb_o_Comp *)tb_nn(v_c, 151))->q_out, ""));
    if (tb_err != 0) {
        const char * const v_err = tb_err;
        tb_err = 0;
        (tb_show_4(({ const char *tb_c61_0 = "не могу записать файл: "; long long tb_c61_0n = sizeof("не могу записать файл: ") - 1; const char *tb_c61_1 = v_c_path; long long tb_c61_1n = (long long)strlen(tb_c61_1); char *tb_c61r = tb_mem(tb_c61_0n + tb_c61_1n + 1), *tb_c61rw = tb_c61r; memcpy(tb_c61rw, tb_c61_0, tb_c61_0n); tb_c61rw += tb_c61_0n; memcpy(tb_c61rw, tb_c61_1, tb_c61_1n); tb_c61rw += tb_c61_1n; *tb_c61rw = 0; (const char *)tb_c61r; })), tb_ps("\n"));
        return 1LL;
    }
    const char * v_script = "ENTRY(_start)\nSECTIONS {\n  . = 1M;\n  .text : { KEEP(*(.multiboot)) *(.text*) }\n"; tb_rl_script = -1;
    tb_app_n(&v_script, &tb_rl_script, &tb_rc_script, "  .rodata : { *(.rodata*) }\n  .data : { *(.data*) }\n  .bss : { *(COMMON) *(.bss*) }\n}\n", sizeof("  .rodata : { *(.rodata*) }\n  .data : { *(.data*) }\n  .bss : { *(COMMON) *(.bss*) }\n}\n") - 1); 
    tb_write_file(v_ld_path, (tb_rl_script = -1, v_script));
    if (tb_err != 0) {
        const char * const v_err = tb_err;
        tb_err = 0;
        (tb_show_4(({ const char *tb_c63_0 = "не могу записать файл: "; long long tb_c63_0n = sizeof("не могу записать файл: ") - 1; const char *tb_c63_1 = v_ld_path; long long tb_c63_1n = (long long)strlen(tb_c63_1); char *tb_c63r = tb_mem(tb_c63_0n + tb_c63_1n + 1), *tb_c63rw = tb_c63r; memcpy(tb_c63rw, tb_c63_0, tb_c63_0n); tb_c63rw += tb_c63_0n; memcpy(tb_c63rw, tb_c63_1, tb_c63_1n); tb_c63rw += tb_c63_1n; *tb_c63rw = 0; (const char *)tb_c63r; })), tb_ps("\n"));
        return 1LL;
    }
    const char * v_flags = " -ffreestanding -nostdlib -fno-pie -fno-stack-protector -fno-builtin -O2 -march=i686 -mno-sse -mno-mmx -w -c ";
    const char * v_o_path = f_main__quote(({ const char *tb_c64_0 = v_exe; long long tb_c64_0n = (long long)strlen(tb_c64_0); const char *tb_c64_1 = ".o"; long long tb_c64_1n = sizeof(".o") - 1; char *tb_c64r = tb_mem(tb_c64_0n + tb_c64_1n + 1), *tb_c64rw = tb_c64r; memcpy(tb_c64rw, tb_c64_0, tb_c64_0n); tb_c64rw += tb_c64_0n; memcpy(tb_c64rw, tb_c64_1, tb_c64_1n); tb_c64rw += tb_c64_1n; *tb_c64rw = 0; (const char *)tb_c64r; }));
    const char * v_cc = ({ const char *tb_c65_0 = "if command -v clang >/dev/null 2>&1; then clang --target=i686-elf"; long long tb_c65_0n = sizeof("if command -v clang >/dev/null 2>&1; then clang --target=i686-elf") - 1; const char *tb_c65_1 = v_flags; long long tb_c65_1n = (long long)strlen(tb_c65_1); const char *tb_c65_2 = f_main__quote(v_c_path); long long tb_c65_2n = (long long)strlen(tb_c65_2); const char *tb_c65_3 = " -o "; long long tb_c65_3n = sizeof(" -o ") - 1; const char *tb_c65_4 = v_o_path; long long tb_c65_4n = (long long)strlen(tb_c65_4); char *tb_c65r = tb_mem(tb_c65_0n + tb_c65_1n + tb_c65_2n + tb_c65_3n + tb_c65_4n + 1), *tb_c65rw = tb_c65r; memcpy(tb_c65rw, tb_c65_0, tb_c65_0n); tb_c65rw += tb_c65_0n; memcpy(tb_c65rw, tb_c65_1, tb_c65_1n); tb_c65rw += tb_c65_1n; memcpy(tb_c65rw, tb_c65_2, tb_c65_2n); tb_c65rw += tb_c65_2n; memcpy(tb_c65rw, tb_c65_3, tb_c65_3n); tb_c65rw += tb_c65_3n; memcpy(tb_c65rw, tb_c65_4, tb_c65_4n); tb_c65rw += tb_c65_4n; *tb_c65rw = 0; (const char *)tb_c65r; }); tb_rl_cc = -1;
    tb_app_n(&v_cc, &tb_rl_cc, &tb_rc_cc, "; else gcc -m32", sizeof("; else gcc -m32") - 1); tb_app(&v_cc, &tb_rl_cc, &tb_rc_cc, v_flags); tb_app(&v_cc, &tb_rl_cc, &tb_rc_cc, f_main__quote(v_c_path)); tb_app_n(&v_cc, &tb_rl_cc, &tb_rc_cc, " -o ", sizeof(" -o ") - 1); tb_app(&v_cc, &tb_rl_cc, &tb_rc_cc, v_o_path); tb_app_n(&v_cc, &tb_rl_cc, &tb_rc_cc, "; fi", sizeof("; fi") - 1); 
    if ((tb_run((tb_rl_cc = -1, v_cc)) != 0LL)) {
        (tb_show_4(({ const char *tb_c66_0 = "компилятор C не справился (это ошибка Tachbit, а не твоя): "; long long tb_c66_0n = sizeof("компилятор C не справился (это ошибка Tachbit, а не твоя): ") - 1; const char *tb_c66_1 = v_c_path; long long tb_c66_1n = (long long)strlen(tb_c66_1); char *tb_c66r = tb_mem(tb_c66_0n + tb_c66_1n + 1), *tb_c66rw = tb_c66r; memcpy(tb_c66rw, tb_c66_0, tb_c66_0n); tb_c66rw += tb_c66_0n; memcpy(tb_c66rw, tb_c66_1, tb_c66_1n); tb_c66rw += tb_c66_1n; *tb_c66rw = 0; (const char *)tb_c66r; })), tb_ps("\n"));
        return 1LL;
    }
    const char * v_ld_cmd = ({ const char *tb_c67_0 = "if command -v ld.lld >/dev/null 2>&1; then L=ld.lld; else L=ld; fi; $L -m elf_i386 -T "; long long tb_c67_0n = sizeof("if command -v ld.lld >/dev/null 2>&1; then L=ld.lld; else L=ld; fi; $L -m elf_i386 -T ") - 1; const char *tb_c67_1 = f_main__quote(v_ld_path); long long tb_c67_1n = (long long)strlen(tb_c67_1); char *tb_c67r = tb_mem(tb_c67_0n + tb_c67_1n + 1), *tb_c67rw = tb_c67r; memcpy(tb_c67rw, tb_c67_0, tb_c67_0n); tb_c67rw += tb_c67_0n; memcpy(tb_c67rw, tb_c67_1, tb_c67_1n); tb_c67rw += tb_c67_1n; *tb_c67rw = 0; (const char *)tb_c67r; }); tb_rl_ld_cmd = -1;
    tb_app_n(&v_ld_cmd, &tb_rl_ld_cmd, &tb_rc_ld_cmd, " -o ", sizeof(" -o ") - 1); tb_app(&v_ld_cmd, &tb_rl_ld_cmd, &tb_rc_ld_cmd, f_main__quote(v_elf)); tb_app_n(&v_ld_cmd, &tb_rl_ld_cmd, &tb_rc_ld_cmd, " ", sizeof(" ") - 1); tb_app(&v_ld_cmd, &tb_rl_ld_cmd, &tb_rc_ld_cmd, v_o_path); 
    if ((tb_run((tb_rl_ld_cmd = -1, v_ld_cmd)) != 0LL)) {
        (tb_show_4("не получилось собрать ядро: нужен ld.lld (на Mac: brew install lld)"), tb_ps("\n"));
        return 1LL;
    }
    long long v_cleaned = tb_run(({ const char *tb_c68_0 = "rm -f "; long long tb_c68_0n = sizeof("rm -f ") - 1; const char *tb_c68_1 = v_o_path; long long tb_c68_1n = (long long)strlen(tb_c68_1); const char *tb_c68_2 = " "; long long tb_c68_2n = sizeof(" ") - 1; const char *tb_c68_3 = f_main__quote(v_ld_path); long long tb_c68_3n = (long long)strlen(tb_c68_3); char *tb_c68r = tb_mem(tb_c68_0n + tb_c68_1n + tb_c68_2n + tb_c68_3n + 1), *tb_c68rw = tb_c68r; memcpy(tb_c68rw, tb_c68_0, tb_c68_0n); tb_c68rw += tb_c68_0n; memcpy(tb_c68rw, tb_c68_1, tb_c68_1n); tb_c68rw += tb_c68_1n; memcpy(tb_c68rw, tb_c68_2, tb_c68_2n); tb_c68rw += tb_c68_2n; memcpy(tb_c68rw, tb_c68_3, tb_c68_3n); tb_c68rw += tb_c68_3n; *tb_c68rw = 0; (const char *)tb_c68r; }));
    (tb_show_4(({ const char *tb_c69_0 = "готово ядро: "; long long tb_c69_0n = sizeof("готово ядро: ") - 1; const char *tb_c69_1 = v_elf; long long tb_c69_1n = (long long)strlen(tb_c69_1); char *tb_c69r = tb_mem(tb_c69_0n + tb_c69_1n + 1), *tb_c69rw = tb_c69r; memcpy(tb_c69rw, tb_c69_0, tb_c69_0n); tb_c69rw += tb_c69_0n; memcpy(tb_c69rw, tb_c69_1, tb_c69_1n); tb_c69rw += tb_c69_1n; *tb_c69rw = 0; (const char *)tb_c69r; })), tb_ps("\n"));
    (tb_show_4(({ const char *tb_c70_0 = "запуск:  qemu-system-i386 -kernel "; long long tb_c70_0n = sizeof("запуск:  qemu-system-i386 -kernel ") - 1; const char *tb_c70_1 = f_main__quote(v_elf); long long tb_c70_1n = (long long)strlen(tb_c70_1); char *tb_c70r = tb_mem(tb_c70_0n + tb_c70_1n + 1), *tb_c70rw = tb_c70r; memcpy(tb_c70rw, tb_c70_0, tb_c70_0n); tb_c70rw += tb_c70_0n; memcpy(tb_c70rw, tb_c70_1, tb_c70_1n); tb_c70rw += tb_c70_1n; *tb_c70rw = 0; (const char *)tb_c70r; })), tb_ps("\n"));
    return 0LL;
    tb_crash(143, "функция build_bare дошла до конца без return");
    return 0LL;
}

static void f_main__usage(void) {
    (tb_show_4("Tachbit 0.7 (компилятор на Tachbit)\n"), tb_ps("\n"));
    (tb_show_4("  tb new   имя             новый проект: папка имя/ с main.tb"), tb_ps("\n"));
    (tb_show_4("  tb start имя [слова]     собрать проект и запустить (без имени — текущая папка)\n"), tb_ps("\n"));
    (tb_show_4("  tachbit run    файл.tb [слова]   сделать программу и запустить"), tb_ps("\n"));
    (tb_show_4("  tachbit build  файл.tb   сделать программу рядом с файлом"), tb_ps("\n"));
    (tb_show_4("  tachbit bare   файл.tb   сделать ядро без ОС (файл.elf для QEMU)"), tb_ps("\n"));
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
    const char * v_mode = (*(const char * *)tb_at(v_a, 0LL, sizeof(const char *), 194));
    if (({ const char *tb_s71 = v_mode; ((unsigned char)tb_s71[0] == 110 && (unsigned char)tb_s71[1] == 101 && (unsigned char)tb_s71[2] == 119 && tb_s71[3] == 0); })) {
        const char * v_name = "";
        if (((v_a)->len > 1LL)) {
            v_name = (*(const char * *)tb_at(v_a, 1LL, sizeof(const char *), 198));
        }
        f_main__new_project(v_name);
        return;
    }
    if (((({ const char *tb_s72 = v_mode; ((unsigned char)tb_s72[0] == 104 && (unsigned char)tb_s72[1] == 101 && (unsigned char)tb_s72[2] == 108 && (unsigned char)tb_s72[3] == 112 && tb_s72[4] == 0); }) || ({ const char *tb_s73 = v_mode; ((unsigned char)tb_s73[0] == 45 && (unsigned char)tb_s73[1] == 104 && tb_s73[2] == 0); })) || ({ const char *tb_s74 = v_mode; ((unsigned char)tb_s74[0] == 45 && (unsigned char)tb_s74[1] == 45 && (unsigned char)tb_s74[2] == 104 && (unsigned char)tb_s74[3] == 101 && (unsigned char)tb_s74[4] == 108 && (unsigned char)tb_s74[5] == 112 && tb_s74[6] == 0); }))) {
        f_main__usage();
        return;
    }
    if ((({ const char *tb_s75 = v_mode; ((unsigned char)tb_s75[0] == 118 && (unsigned char)tb_s75[1] == 101 && (unsigned char)tb_s75[2] == 114 && (unsigned char)tb_s75[3] == 115 && (unsigned char)tb_s75[4] == 105 && (unsigned char)tb_s75[5] == 111 && (unsigned char)tb_s75[6] == 110 && tb_s75[7] == 0); }) || (strcmp(v_mode, "--version") == 0))) {
        (tb_show_4("Tachbit 0.7 (компилятор на Tachbit)"), tb_ps("\n"));
        return;
    }
    if (({ const char *tb_s76 = v_mode; ((unsigned char)tb_s76[0] == 115 && (unsigned char)tb_s76[1] == 116 && (unsigned char)tb_s76[2] == 97 && (unsigned char)tb_s76[3] == 114 && (unsigned char)tb_s76[4] == 116 && tb_s76[5] == 0); })) {
        v_mode = "run";
    }
    if ((((v_a)->len < 2LL) && (!((((({ const char *tb_s77 = v_mode; ((unsigned char)tb_s77[0] == 114 && (unsigned char)tb_s77[1] == 117 && (unsigned char)tb_s77[2] == 110 && tb_s77[3] == 0); }) || ({ const char *tb_s78 = v_mode; ((unsigned char)tb_s78[0] == 98 && (unsigned char)tb_s78[1] == 117 && (unsigned char)tb_s78[2] == 105 && (unsigned char)tb_s78[3] == 108 && (unsigned char)tb_s78[4] == 100 && tb_s78[5] == 0); })) || ({ const char *tb_s79 = v_mode; ((unsigned char)tb_s79[0] == 98 && (unsigned char)tb_s79[1] == 97 && (unsigned char)tb_s79[2] == 114 && (unsigned char)tb_s79[3] == 101 && tb_s79[4] == 0); })) || ({ const char *tb_s80 = v_mode; ((unsigned char)tb_s80[0] == 99 && (unsigned char)tb_s80[1] == 104 && (unsigned char)tb_s80[2] == 101 && (unsigned char)tb_s80[3] == 99 && (unsigned char)tb_s80[4] == 107 && tb_s80[5] == 0); })) || ({ const char *tb_s81 = v_mode; ((unsigned char)tb_s81[0] == 99 && tb_s81[1] == 0); }))))) {
        f_main__usage();
        tb_exit(1LL);
    }
    const char * v_arg = "";
    if (((v_a)->len > 1LL)) {
        v_arg = (*(const char * *)tb_at(v_a, 1LL, sizeof(const char *), 215));
    }
    const char * v_path;
    const char * v_exe;
    {
        tb_r_f_main__project_main tb_m82 = f_main__project_main(v_arg);
        v_path = tb_m82.o0;
        v_exe = tb_m82.o1;
    }
    tb_o_Comp * v_c = tb_new_Comp(v_path, tb_list_new(sizeof(tb_o_Token *)), 0LL, tb_list_new(sizeof(tb_o_Type *)), tb_list_new(sizeof(tb_o_Func *)), tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Use *)), tb_list_new(sizeof(const char *)), "", tb_list_new(sizeof(tb_o_Var *)), 0LL, 0LL, 0LL, 0LL, tb_list_new(sizeof(long long)), 0LL, 0, 0LL, 0LL, 0, 0, tb_list_new(sizeof(const char *)), 0LL, 0, tb_list_new(sizeof(const char *)), tb_list_new(sizeof(const char *)), tb_list_new(sizeof(const char *)), 0LL, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(const char *)), tb_list_new(sizeof(long long)), 0LL, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(long long)), tb_bits_new(), tb_list_new(sizeof(long long)), tb_list_new(sizeof(long long)), tb_bits_new(), tb_list_new(sizeof(const char *)), "", 0, 0);
    if (({ const char *tb_s83 = v_mode; ((unsigned char)tb_s83[0] == 116 && (unsigned char)tb_s83[1] == 111 && (unsigned char)tb_s83[2] == 107 && (unsigned char)tb_s83[3] == 101 && (unsigned char)tb_s83[4] == 110 && (unsigned char)tb_s83[5] == 115 && tb_s83[6] == 0); })) {
        const char * tb_v84 = tb_read_file(v_path);
        if (tb_err != 0) {
            const char * const v_err = tb_err;
            tb_err = 0;
            f_main__say_error(({ const char *tb_c85_0 = "не могу открыть файл: "; long long tb_c85_0n = sizeof("не могу открыть файл: ") - 1; const char *tb_c85_1 = v_path; long long tb_c85_1n = (long long)strlen(tb_c85_1); char *tb_c85r = tb_mem(tb_c85_0n + tb_c85_1n + 1), *tb_c85rw = tb_c85r; memcpy(tb_c85rw, tb_c85_0, tb_c85_0n); tb_c85rw += tb_c85_0n; memcpy(tb_c85rw, tb_c85_1, tb_c85_1n); tb_c85rw += tb_c85_1n; *tb_c85rw = 0; (const char *)tb_c85r; }));
        }
        const char * v_src = tb_v84;
        f_lexer__lex(v_c, v_src);
        f_lexer__print_tokens(v_c);
        return;
    }
    if (({ const char *tb_s86 = v_mode; ((unsigned char)tb_s86[0] == 116 && (unsigned char)tb_s86[1] == 114 && (unsigned char)tb_s86[2] == 101 && (unsigned char)tb_s86[3] == 101 && tb_s86[4] == 0); })) {
        tb_list * v_out = tb_list_new(sizeof(const char *));
        f_parser__tree_lines(f_main__load_program(v_c, v_path), 0LL, v_out);
        (tb_show_4(tb_join(v_out, "\n")), tb_ps("\n"));
        return;
    }
    if (({ const char *tb_s87 = v_mode; ((unsigned char)tb_s87[0] == 99 && (unsigned char)tb_s87[1] == 104 && (unsigned char)tb_s87[2] == 101 && (unsigned char)tb_s87[3] == 99 && (unsigned char)tb_s87[4] == 107 && tb_s87[5] == 0); })) {
        f_check__check(v_c, f_main__load_program(v_c, v_path));
        (tb_show_4("ошибок нет"), tb_ps("\n"));
        return;
    }
    if (({ const char *tb_s88 = v_mode; ((unsigned char)tb_s88[0] == 99 && tb_s88[1] == 0); })) {
        tb_o_Node * v_program = f_main__load_program(v_c, v_path);
        f_check__check(v_c, v_program);
        f_emit__emit_c(v_c, v_program);
        (tb_show_4(tb_cut(tb_join(((tb_o_Comp *)tb_nn(v_c, 237))->q_out, ""), 0LL, (tb_tlen(tb_join(((tb_o_Comp *)tb_nn(v_c, 237))->q_out, "")) - 2LL), 237)), tb_ps("\n"));
        return;
    }
    if (({ const char *tb_s89 = v_mode; ((unsigned char)tb_s89[0] == 98 && (unsigned char)tb_s89[1] == 117 && (unsigned char)tb_s89[2] == 105 && (unsigned char)tb_s89[3] == 108 && (unsigned char)tb_s89[4] == 100 && tb_s89[5] == 0); })) {
        tb_exit(f_main__build(v_c, v_path, v_exe));
    }
    if (({ const char *tb_s90 = v_mode; ((unsigned char)tb_s90[0] == 98 && (unsigned char)tb_s90[1] == 97 && (unsigned char)tb_s90[2] == 114 && (unsigned char)tb_s90[3] == 101 && tb_s90[4] == 0); })) {
        tb_exit(f_main__build_bare(v_c, v_path, v_exe));
    }
    if (({ const char *tb_s91 = v_mode; ((unsigned char)tb_s91[0] == 114 && (unsigned char)tb_s91[1] == 117 && (unsigned char)tb_s91[2] == 110 && tb_s91[3] == 0); })) {
        if ((f_main__build(v_c, v_path, v_exe) != 0LL)) {
            tb_exit(1LL);
        }
        const char * v_command = f_main__quote(v_exe); tb_rl_command = -1;
        if ((tb_find(v_exe, "/") < 0LL)) {
            v_command = f_main__quote(({ const char *tb_c92_0 = "./"; long long tb_c92_0n = sizeof("./") - 1; const char *tb_c92_1 = v_exe; long long tb_c92_1n = (long long)strlen(tb_c92_1); char *tb_c92r = tb_mem(tb_c92_0n + tb_c92_1n + 1), *tb_c92rw = tb_c92r; memcpy(tb_c92rw, tb_c92_0, tb_c92_0n); tb_c92rw += tb_c92_0n; memcpy(tb_c92rw, tb_c92_1, tb_c92_1n); tb_c92rw += tb_c92_1n; *tb_c92rw = 0; (const char *)tb_c92r; })); tb_rl_command = -1;
        }
        {
            long long tb_end93 = ((v_a)->len - 1LL);
            for (long long v_i = 2LL; v_i <= tb_end93; v_i++) {
                tb_app_n(&v_command, &tb_rl_command, &tb_rc_command, " ", sizeof(" ") - 1); tb_app(&v_command, &tb_rl_command, &tb_rc_command, f_main__quote((*(const char * *)tb_at(v_a, v_i, sizeof(const char *), 251)))); 
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
        v_place = ({ const char *tb_c94_0 = ((tb_o_Comp *)tb_nn(v_c, 9))->q_file_name; long long tb_c94_0n = (long long)strlen(tb_c94_0); const char *tb_c94_1 = ", строка "; long long tb_c94_1n = sizeof(", строка ") - 1; char tb_c94_2d[24]; long long tb_c94_2n; const char *tb_c94_2 = tb_digits(v_line, tb_c94_2d, &tb_c94_2n); char *tb_c94r = tb_mem(tb_c94_0n + tb_c94_1n + tb_c94_2n + 1), *tb_c94rw = tb_c94r; memcpy(tb_c94rw, tb_c94_0, tb_c94_0n); tb_c94rw += tb_c94_0n; memcpy(tb_c94rw, tb_c94_1, tb_c94_1n); tb_c94rw += tb_c94_1n; memcpy(tb_c94rw, tb_c94_2, tb_c94_2n); tb_c94rw += tb_c94_2n; *tb_c94rw = 0; (const char *)tb_c94r; });
    }
    const char * v_s = ({ const char *tb_c95_0 = v_place; long long tb_c95_0n = (long long)strlen(tb_c95_0); const char *tb_c95_1 = ": "; long long tb_c95_1n = sizeof(": ") - 1; const char *tb_c95_2 = v_message; long long tb_c95_2n = (long long)strlen(tb_c95_2); char *tb_c95r = tb_mem(tb_c95_0n + tb_c95_1n + tb_c95_2n + 1), *tb_c95rw = tb_c95r; memcpy(tb_c95rw, tb_c95_0, tb_c95_0n); tb_c95rw += tb_c95_0n; memcpy(tb_c95rw, tb_c95_1, tb_c95_1n); tb_c95rw += tb_c95_1n; memcpy(tb_c95rw, tb_c95_2, tb_c95_2n); tb_c95rw += tb_c95_2n; *tb_c95rw = 0; (const char *)tb_c95r; }); tb_rl_s = -1;
    if (({ const char *tb_s96 = v_detail; !(tb_s96[0] == 0); })) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, ": ", sizeof(": ") - 1); tb_app(&v_s, &tb_rl_s, &tb_rc_s, v_detail); 
    }
    if (v_color) {
        const char * v_esc = tb_chr(27LL);
        v_s = ({ const char *tb_c97_0 = v_esc; long long tb_c97_0n = (long long)strlen(tb_c97_0); const char *tb_c97_1 = "[1m"; long long tb_c97_1n = sizeof("[1m") - 1; const char *tb_c97_2 = v_place; long long tb_c97_2n = (long long)strlen(tb_c97_2); const char *tb_c97_3 = ":"; long long tb_c97_3n = sizeof(":") - 1; const char *tb_c97_4 = v_esc; long long tb_c97_4n = (long long)strlen(tb_c97_4); const char *tb_c97_5 = "[0m "; long long tb_c97_5n = sizeof("[0m ") - 1; const char *tb_c97_6 = v_esc; long long tb_c97_6n = (long long)strlen(tb_c97_6); const char *tb_c97_7 = "[31m"; long long tb_c97_7n = sizeof("[31m") - 1; const char *tb_c97_8 = v_message; long long tb_c97_8n = (long long)strlen(tb_c97_8); const char *tb_c97_9 = v_esc; long long tb_c97_9n = (long long)strlen(tb_c97_9); const char *tb_c97_10 = "[0m"; long long tb_c97_10n = sizeof("[0m") - 1; char *tb_c97r = tb_mem(tb_c97_0n + tb_c97_1n + tb_c97_2n + tb_c97_3n + tb_c97_4n + tb_c97_5n + tb_c97_6n + tb_c97_7n + tb_c97_8n + tb_c97_9n + tb_c97_10n + 1), *tb_c97rw = tb_c97r; memcpy(tb_c97rw, tb_c97_0, tb_c97_0n); tb_c97rw += tb_c97_0n; memcpy(tb_c97rw, tb_c97_1, tb_c97_1n); tb_c97rw += tb_c97_1n; memcpy(tb_c97rw, tb_c97_2, tb_c97_2n); tb_c97rw += tb_c97_2n; memcpy(tb_c97rw, tb_c97_3, tb_c97_3n); tb_c97rw += tb_c97_3n; memcpy(tb_c97rw, tb_c97_4, tb_c97_4n); tb_c97rw += tb_c97_4n; memcpy(tb_c97rw, tb_c97_5, tb_c97_5n); tb_c97rw += tb_c97_5n; memcpy(tb_c97rw, tb_c97_6, tb_c97_6n); tb_c97rw += tb_c97_6n; memcpy(tb_c97rw, tb_c97_7, tb_c97_7n); tb_c97rw += tb_c97_7n; memcpy(tb_c97rw, tb_c97_8, tb_c97_8n); tb_c97rw += tb_c97_8n; memcpy(tb_c97rw, tb_c97_9, tb_c97_9n); tb_c97rw += tb_c97_9n; memcpy(tb_c97rw, tb_c97_10, tb_c97_10n); tb_c97rw += tb_c97_10n; *tb_c97rw = 0; (const char *)tb_c97r; }); tb_rl_s = -1;
        if (({ const char *tb_s98 = v_detail; !(tb_s98[0] == 0); })) {
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
        tb_list * tb_l99 = v_commands;
        for (long long tb_i99 = 0; tb_i99 < tb_l99->len; tb_i99++) {
            const char * const v_c = ((const char * *)tb_l99->data)[tb_i99];
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
    { tb_o_Token * tb_v100 = tb_new_Token(v_kind, v_word, v_line); *(tb_o_Token * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 26))->q_tokens) = tb_v100; }
}

static _Bool f_lexer__two_char_sign(const char * v_a, const char * v_b) {
    if ((({ const char *tb_s101 = v_b; ((unsigned char)tb_s101[0] == 61 && tb_s101[1] == 0); }) && (((((((({ const char *tb_s102 = v_a; ((unsigned char)tb_s102[0] == 43 && tb_s102[1] == 0); }) || ({ const char *tb_s103 = v_a; ((unsigned char)tb_s103[0] == 45 && tb_s103[1] == 0); })) || ({ const char *tb_s104 = v_a; ((unsigned char)tb_s104[0] == 42 && tb_s104[1] == 0); })) || ({ const char *tb_s105 = v_a; ((unsigned char)tb_s105[0] == 47 && tb_s105[1] == 0); })) || ({ const char *tb_s106 = v_a; ((unsigned char)tb_s106[0] == 61 && tb_s106[1] == 0); })) || ({ const char *tb_s107 = v_a; ((unsigned char)tb_s107[0] == 33 && tb_s107[1] == 0); })) || ({ const char *tb_s108 = v_a; ((unsigned char)tb_s108[0] == 60 && tb_s108[1] == 0); })) || ({ const char *tb_s109 = v_a; ((unsigned char)tb_s109[0] == 62 && tb_s109[1] == 0); })))) {
        return 1;
    }
    if ((({ const char *tb_s110 = v_a; ((unsigned char)tb_s110[0] == 45 && tb_s110[1] == 0); }) && ({ const char *tb_s111 = v_b; ((unsigned char)tb_s111[0] == 62 && tb_s111[1] == 0); }))) {
        return 1;
    }
    if ((({ const char *tb_s112 = v_a; ((unsigned char)tb_s112[0] == 46 && tb_s112[1] == 0); }) && ({ const char *tb_s113 = v_b; ((unsigned char)tb_s113[0] == 46 && tb_s113[1] == 0); }))) {
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
    { tb_list * tb_v114 = v_fresh; tb_list * *tb_p114 = &(((tb_o_Comp *)tb_nn(v_c, 43))->q_tokens); *tb_p114 = tb_v114; }
    tb_list * v_indents = tb_list_from(1, sizeof(long long), (long long[]){0LL});
    tb_list * v_lines = tb_split(v_src, "\n", 45);
    long long v_line = 0LL;
    {
        tb_list * tb_l115 = v_lines;
        for (long long tb_i115 = 0; tb_i115 < tb_l115->len; tb_i115++) {
            const char * const v_s = ((const char * *)tb_l115->data)[tb_i115];
            { long long tb_v116 = 1LL; long long *tb_p116 = &(v_line); *tb_p116 += tb_v116; }
            long long v_n = tb_tlen(v_s);
            long long v_spaces = 0LL;
            long long v_pos = 0LL;
            while (((v_pos < v_n) && ((f_lexer__at(v_s, v_pos) == 32LL) || (f_lexer__at(v_s, v_pos) == 9LL)))) {
                tb_mark tb_am117 = tb_temp_begin();
                do {
                    if ((f_lexer__at(v_s, v_pos) == 9LL)) {
                        { long long tb_v118 = 4LL; long long *tb_p118 = &(v_spaces); *tb_p118 += tb_v118; }
                    } else {
                        { long long tb_v119 = 1LL; long long *tb_p119 = &(v_spaces); *tb_p119 += tb_v119; }
                    }
                    { long long tb_v120 = 1LL; long long *tb_p120 = &(v_pos); *tb_p120 += tb_v120; }
                } while (0);
                tb_temp_end(tb_am117);
            }
            if ((((v_pos >= v_n) || (f_lexer__at(v_s, v_pos) == 13LL)) || ((f_lexer__at(v_s, v_pos) == 47LL) && (f_lexer__at(v_s, (v_pos + 1LL)) == 47LL)))) {
                continue;
            }
            long long v_top = (*(long long *)tb_at(v_indents, ((v_indents)->len - 1LL), sizeof(long long), 62));
            if ((v_spaces > v_top)) {
                if (((v_indents)->len == 200LL)) {
                    f_errors__error_at(v_c, v_line, "слишком глубокая вложенность", "");
                }
                { long long tb_v121 = v_spaces; *(long long *)tb_grow(v_indents) = tb_v121; }
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
                    { long long tb_v122 = 1LL; long long *tb_p122 = &(v_pos); *tb_p122 += tb_v122; }
                    continue;
                }
                if (((v_ch == 47LL) && (f_lexer__at(v_s, (v_pos + 1LL)) == 47LL))) {
                    break;
                }
                long long v_start = v_pos;
                if (f_lexer__is_letter(v_ch)) {
                    while ((f_lexer__is_letter(f_lexer__at(v_s, v_pos)) || f_lexer__is_digit(f_lexer__at(v_s, v_pos)))) {
                        { long long tb_v123 = 1LL; long long *tb_p123 = &(v_pos); *tb_p123 += tb_v123; }
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
                        { long long tb_v124 = 1LL; long long *tb_p124 = &(v_pos); *tb_p124 += tb_v124; }
                    }
                    if (((f_lexer__at(v_s, v_pos) == 46LL) && f_lexer__is_digit(f_lexer__at(v_s, (v_pos + 1LL))))) {
                        v_kind = "decimal";
                        { long long tb_v125 = 1LL; long long *tb_p125 = &(v_pos); *tb_p125 += tb_v125; }
                        while (f_lexer__is_digit(f_lexer__at(v_s, v_pos))) {
                            { long long tb_v126 = 1LL; long long *tb_p126 = &(v_pos); *tb_p126 += tb_v126; }
                        }
                    }
                    f_lexer__add_token(v_c, v_kind, tb_cut(v_s, v_start, (v_pos - 1LL), 101), v_line);
                } else if ((v_ch == 34LL)) {
                    { long long tb_v127 = 1LL; long long *tb_p127 = &(v_pos); *tb_p127 += tb_v127; }
                    while (((v_pos < v_n) && (f_lexer__at(v_s, v_pos) != 34LL))) {
                        tb_mark tb_am128 = tb_temp_begin();
                        do {
                            if ((f_lexer__at(v_s, v_pos) == 92LL)) {
                                { long long tb_v129 = 1LL; long long *tb_p129 = &(v_pos); *tb_p129 += tb_v129; }
                            }
                            { long long tb_v130 = 1LL; long long *tb_p130 = &(v_pos); *tb_p130 += tb_v130; }
                        } while (0);
                        tb_temp_end(tb_am128);
                    }
                    if ((f_lexer__at(v_s, v_pos) != 34LL)) {
                        f_errors__error_at(v_c, v_line, "текст в кавычках не закрыт", "");
                    }
                    { long long tb_v131 = 1LL; long long *tb_p131 = &(v_pos); *tb_p131 += tb_v131; }
                    f_lexer__add_token(v_c, "string", tb_cut(v_s, v_start, (v_pos - 1LL), 111), v_line);
                } else {
                    const char * v_a = tb_char_at(v_s, v_pos, -1, 113);
                    const char * v_b = "";
                    if (((v_pos + 1LL) < v_n)) {
                        v_b = tb_char_at(v_s, (v_pos + 1LL), -1, 116);
                    }
                    if (f_lexer__two_char_sign(v_a, v_b)) {
                        f_lexer__add_token(v_c, "sign", ({ const char *tb_c132_0 = v_a; long long tb_c132_0n = (long long)strlen(tb_c132_0); const char *tb_c132_1 = v_b; long long tb_c132_1n = (long long)strlen(tb_c132_1); char *tb_c132r = tb_mem(tb_c132_0n + tb_c132_1n + 1), *tb_c132rw = tb_c132r; memcpy(tb_c132rw, tb_c132_0, tb_c132_0n); tb_c132rw += tb_c132_0n; memcpy(tb_c132rw, tb_c132_1, tb_c132_1n); tb_c132rw += tb_c132_1n; *tb_c132rw = 0; (const char *)tb_c132r; }), v_line);
                        { long long tb_v133 = 2LL; long long *tb_p133 = &(v_pos); *tb_p133 += tb_v133; }
                    } else if (f_lexer__one_char_sign(v_a)) {
                        f_lexer__add_token(v_c, "sign", v_a, v_line);
                        { long long tb_v134 = 1LL; long long *tb_p134 = &(v_pos); *tb_p134 += tb_v134; }
                    } else {
                        f_errors__error_at(v_c, v_line, "непонятный символ", v_a);
                    }
                }
            }
            f_lexer__add_token(v_c, "newline", "", v_line);
        }
    }
    if (((tb_tlen(v_src) > 0LL) && tb_ends(v_src, "\n"))) {
        { long long tb_v135 = 1LL; long long *tb_p135 = &(v_line); *tb_p135 -= tb_v135; }
    }
    while (((v_indents)->len > 1LL)) {
        long long v_gone = (*(long long *)tb_pop(v_indents, 130));
        f_lexer__add_token(v_c, "dedent", "", (v_line + 1LL));
    }
    f_lexer__add_token(v_c, "end", "", (v_line + 1LL));
}

static const char * f_lexer__token_kind_name(const char * v_kind) {
    if (({ const char *tb_s136 = v_kind; ((unsigned char)tb_s136[0] == 110 && (unsigned char)tb_s136[1] == 97 && (unsigned char)tb_s136[2] == 109 && (unsigned char)tb_s136[3] == 101 && tb_s136[4] == 0); })) {
        return "ИМЯ";
    }
    if (({ const char *tb_s137 = v_kind; ((unsigned char)tb_s137[0] == 99 && (unsigned char)tb_s137[1] == 111 && (unsigned char)tb_s137[2] == 109 && (unsigned char)tb_s137[3] == 109 && (unsigned char)tb_s137[4] == 97 && (unsigned char)tb_s137[5] == 110 && (unsigned char)tb_s137[6] == 100 && tb_s137[7] == 0); })) {
        return "КОМАНДА";
    }
    if (({ const char *tb_s138 = v_kind; ((unsigned char)tb_s138[0] == 110 && (unsigned char)tb_s138[1] == 117 && (unsigned char)tb_s138[2] == 109 && (unsigned char)tb_s138[3] == 98 && (unsigned char)tb_s138[4] == 101 && (unsigned char)tb_s138[5] == 114 && tb_s138[6] == 0); })) {
        return "ЧИСЛО";
    }
    if (({ const char *tb_s139 = v_kind; ((unsigned char)tb_s139[0] == 100 && (unsigned char)tb_s139[1] == 101 && (unsigned char)tb_s139[2] == 99 && (unsigned char)tb_s139[3] == 105 && (unsigned char)tb_s139[4] == 109 && (unsigned char)tb_s139[5] == 97 && (unsigned char)tb_s139[6] == 108 && tb_s139[7] == 0); })) {
        return "ДРОБЬ";
    }
    if (({ const char *tb_s140 = v_kind; ((unsigned char)tb_s140[0] == 115 && (unsigned char)tb_s140[1] == 116 && (unsigned char)tb_s140[2] == 114 && (unsigned char)tb_s140[3] == 105 && (unsigned char)tb_s140[4] == 110 && (unsigned char)tb_s140[5] == 103 && tb_s140[6] == 0); })) {
        return "ТЕКСТ";
    }
    if (({ const char *tb_s141 = v_kind; ((unsigned char)tb_s141[0] == 115 && (unsigned char)tb_s141[1] == 105 && (unsigned char)tb_s141[2] == 103 && (unsigned char)tb_s141[3] == 110 && tb_s141[4] == 0); })) {
        return "ЗНАК";
    }
    if (({ const char *tb_s142 = v_kind; ((unsigned char)tb_s142[0] == 110 && (unsigned char)tb_s142[1] == 101 && (unsigned char)tb_s142[2] == 119 && (unsigned char)tb_s142[3] == 108 && (unsigned char)tb_s142[4] == 105 && (unsigned char)tb_s142[5] == 110 && (unsigned char)tb_s142[6] == 101 && tb_s142[7] == 0); })) {
        return "КОНЕЦ СТРОКИ";
    }
    if (({ const char *tb_s143 = v_kind; ((unsigned char)tb_s143[0] == 105 && (unsigned char)tb_s143[1] == 110 && (unsigned char)tb_s143[2] == 100 && (unsigned char)tb_s143[3] == 101 && (unsigned char)tb_s143[4] == 110 && (unsigned char)tb_s143[5] == 116 && tb_s143[6] == 0); })) {
        return "ОТСТУП";
    }
    if (({ const char *tb_s144 = v_kind; ((unsigned char)tb_s144[0] == 100 && (unsigned char)tb_s144[1] == 101 && (unsigned char)tb_s144[2] == 100 && (unsigned char)tb_s144[3] == 101 && (unsigned char)tb_s144[4] == 110 && (unsigned char)tb_s144[5] == 116 && tb_s144[6] == 0); })) {
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
        tb_list * tb_l145 = ((tb_o_Comp *)tb_nn(v_c, 158))->q_tokens;
        for (long long tb_i145 = 0; tb_i145 < tb_l145->len; tb_i145++) {
            tb_o_Token * const v_t = ((tb_o_Token * *)tb_l145->data)[tb_i145];
            const char * v_num = ({ const char *tb_c146_0 = ""; long long tb_c146_0n = sizeof("") - 1; char tb_c146_1d[24]; long long tb_c146_1n; const char *tb_c146_1 = tb_digits(((tb_o_Token *)tb_nn(v_t, 159))->q_line, tb_c146_1d, &tb_c146_1n); char *tb_c146r = tb_mem(tb_c146_0n + tb_c146_1n + 1), *tb_c146rw = tb_c146r; memcpy(tb_c146rw, tb_c146_0, tb_c146_0n); tb_c146rw += tb_c146_0n; memcpy(tb_c146rw, tb_c146_1, tb_c146_1n); tb_c146rw += tb_c146_1n; *tb_c146rw = 0; (const char *)tb_c146r; });
            while ((tb_tlen(v_num) < 3LL)) {
                v_num = ({ const char *tb_c147_0 = " "; long long tb_c147_0n = sizeof(" ") - 1; const char *tb_c147_1 = v_num; long long tb_c147_1n = (long long)strlen(tb_c147_1); char *tb_c147r = tb_mem(tb_c147_0n + tb_c147_1n + 1), *tb_c147rw = tb_c147r; memcpy(tb_c147rw, tb_c147_0, tb_c147_0n); tb_c147rw += tb_c147_0n; memcpy(tb_c147rw, tb_c147_1, tb_c147_1n); tb_c147rw += tb_c147_1n; *tb_c147rw = 0; (const char *)tb_c147r; });
            }
            const char * v_name = f_lexer__token_kind_name(((tb_o_Token *)tb_nn(v_t, 162))->q_kind); tb_rl_name = -1;
            while (((tb_rl_name >= 0 ? tb_rl_name : tb_tlen(v_name)) < 13LL)) {
                tb_app_n(&v_name, &tb_rl_name, &tb_rc_name, " ", sizeof(" ") - 1); 
            }
            { const char * tb_v148 = ({ const char *tb_c149_0 = v_num; long long tb_c149_0n = (long long)strlen(tb_c149_0); const char *tb_c149_1 = ": "; long long tb_c149_1n = sizeof(": ") - 1; const char *tb_c149_2 = v_name; long long tb_c149_2n = (long long)strlen(tb_c149_2); const char *tb_c149_3 = " "; long long tb_c149_3n = sizeof(" ") - 1; const char *tb_c149_4 = ((tb_o_Token *)tb_nn(v_t, 165))->q_word; long long tb_c149_4n = (long long)strlen(tb_c149_4); char *tb_c149r = tb_mem(tb_c149_0n + tb_c149_1n + tb_c149_2n + tb_c149_3n + tb_c149_4n + 1), *tb_c149rw = tb_c149r; memcpy(tb_c149rw, tb_c149_0, tb_c149_0n); tb_c149rw += tb_c149_0n; memcpy(tb_c149rw, tb_c149_1, tb_c149_1n); tb_c149rw += tb_c149_1n; memcpy(tb_c149rw, tb_c149_2, tb_c149_2n); tb_c149rw += tb_c149_2n; memcpy(tb_c149rw, tb_c149_3, tb_c149_3n); tb_c149rw += tb_c149_3n; memcpy(tb_c149rw, tb_c149_4, tb_c149_4n); tb_c149rw += tb_c149_4n; *tb_c149rw = 0; (const char *)tb_c149r; }); *(const char * *)tb_grow(v_out) = tb_v148; }
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
    { tb_o_Node * tb_v150 = v_kid; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_parent, 11))->q_kids) = tb_v150; }
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
    if (({ const char *tb_s151 = ((tb_o_Token *)tb_nn((*(tb_o_Token * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 27))->q_tokens, ((tb_o_Comp *)tb_nn(v_c, 27))->q_pos, sizeof(tb_o_Token *), 27)), 27))->q_kind; !((unsigned char)tb_s151[0] == 101 && (unsigned char)tb_s151[1] == 110 && (unsigned char)tb_s151[2] == 100 && tb_s151[3] == 0); })) {
        { long long tb_v152 = 1LL; long long *tb_p152 = &(((tb_o_Comp *)tb_nn(v_c, 28))->q_pos); *tb_p152 += tb_v152; }
    }
}

static _Bool f_parser__is_sign(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__now(v_c);
    return (({ const char *tb_s153 = ((tb_o_Token *)tb_nn(v_t, 32))->q_kind; ((unsigned char)tb_s153[0] == 115 && (unsigned char)tb_s153[1] == 105 && (unsigned char)tb_s153[2] == 103 && (unsigned char)tb_s153[3] == 110 && tb_s153[4] == 0); }) && (strcmp(((tb_o_Token *)tb_nn(v_t, 32))->q_word, v_s) == 0));
    tb_crash(30, "функция is_sign дошла до конца без return");
    return 0;
}

static _Bool f_parser__is_cmd(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__now(v_c);
    return (({ const char *tb_s154 = ((tb_o_Token *)tb_nn(v_t, 36))->q_kind; ((unsigned char)tb_s154[0] == 99 && (unsigned char)tb_s154[1] == 111 && (unsigned char)tb_s154[2] == 109 && (unsigned char)tb_s154[3] == 109 && (unsigned char)tb_s154[4] == 97 && (unsigned char)tb_s154[5] == 110 && (unsigned char)tb_s154[6] == 100 && tb_s154[7] == 0); }) && (strcmp(((tb_o_Token *)tb_nn(v_t, 36))->q_word, v_s) == 0));
    tb_crash(34, "функция is_cmd дошла до конца без return");
    return 0;
}

static _Bool f_parser__ahead_is_sign(tb_o_Comp * v_c, const char * v_s) {
    tb_o_Token * v_t = f_parser__ahead(v_c);
    return (({ const char *tb_s155 = ((tb_o_Token *)tb_nn(v_t, 40))->q_kind; ((unsigned char)tb_s155[0] == 115 && (unsigned char)tb_s155[1] == 105 && (unsigned char)tb_s155[2] == 103 && (unsigned char)tb_s155[3] == 110 && tb_s155[4] == 0); }) && (strcmp(((tb_o_Token *)tb_nn(v_t, 40))->q_word, v_s) == 0));
    tb_crash(38, "функция ahead_is_sign дошла до конца без return");
    return 0;
}

static const char * f_parser__what_is_here(tb_o_Comp * v_c) {
    const char * v_k = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 43))->q_kind;
    if (({ const char *tb_s156 = v_k; ((unsigned char)tb_s156[0] == 110 && (unsigned char)tb_s156[1] == 101 && (unsigned char)tb_s156[2] == 119 && (unsigned char)tb_s156[3] == 108 && (unsigned char)tb_s156[4] == 105 && (unsigned char)tb_s156[5] == 110 && (unsigned char)tb_s156[6] == 101 && tb_s156[7] == 0); })) {
        return "конец строки";
    }
    if (({ const char *tb_s157 = v_k; ((unsigned char)tb_s157[0] == 105 && (unsigned char)tb_s157[1] == 110 && (unsigned char)tb_s157[2] == 100 && (unsigned char)tb_s157[3] == 101 && (unsigned char)tb_s157[4] == 110 && (unsigned char)tb_s157[5] == 116 && tb_s157[6] == 0); })) {
        return "лишний отступ";
    }
    if (({ const char *tb_s158 = v_k; ((unsigned char)tb_s158[0] == 100 && (unsigned char)tb_s158[1] == 101 && (unsigned char)tb_s158[2] == 100 && (unsigned char)tb_s158[3] == 101 && (unsigned char)tb_s158[4] == 110 && (unsigned char)tb_s158[5] == 116 && tb_s158[6] == 0); })) {
        return "конец блока";
    }
    if (({ const char *tb_s159 = v_k; ((unsigned char)tb_s159[0] == 101 && (unsigned char)tb_s159[1] == 110 && (unsigned char)tb_s159[2] == 100 && tb_s159[3] == 0); })) {
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
    if (({ const char *tb_s160 = ((tb_o_Token *)tb_nn(v_t, 66))->q_kind; !((unsigned char)tb_s160[0] == 110 && (unsigned char)tb_s160[1] == 97 && (unsigned char)tb_s160[2] == 109 && (unsigned char)tb_s160[3] == 101 && tb_s160[4] == 0); })) {
        if (({ const char *tb_s161 = ((tb_o_Token *)tb_nn(v_t, 67))->q_kind; ((unsigned char)tb_s161[0] == 99 && (unsigned char)tb_s161[1] == 111 && (unsigned char)tb_s161[2] == 109 && (unsigned char)tb_s161[3] == 109 && (unsigned char)tb_s161[4] == 97 && (unsigned char)tb_s161[5] == 110 && (unsigned char)tb_s161[6] == 100 && tb_s161[7] == 0); })) {
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
    if (({ const char *tb_s162 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 74))->q_kind; !((unsigned char)tb_s162[0] == 110 && (unsigned char)tb_s162[1] == 101 && (unsigned char)tb_s162[2] == 119 && (unsigned char)tb_s162[3] == 108 && (unsigned char)tb_s162[4] == 105 && (unsigned char)tb_s162[5] == 110 && (unsigned char)tb_s162[6] == 101 && tb_s162[7] == 0); })) {
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
        { const char * tb_v163 = "dict"; const char * *tb_p163 = &(((tb_o_Node *)tb_nn(v_t, 86))->q_name); *tb_p163 = tb_v163; }
        f_parser__next(v_c);
        tb_o_Node * v_key = f_parser__parse_type(v_c);
        if ((({ const char *tb_s164 = ((tb_o_Node *)tb_nn(v_key, 89))->q_name; !((unsigned char)tb_s164[0] == 116 && (unsigned char)tb_s164[1] == 101 && (unsigned char)tb_s164[2] == 120 && (unsigned char)tb_s164[3] == 116 && tb_s164[4] == 0); }) && ({ const char *tb_s165 = ((tb_o_Node *)tb_nn(v_key, 89))->q_name; !((unsigned char)tb_s165[0] == 105 && (unsigned char)tb_s165[1] == 110 && (unsigned char)tb_s165[2] == 116 && tb_s165[3] == 0); }))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_key, 90))->q_line, "ключ у dict бывает text или int", ((tb_o_Node *)tb_nn(v_key, 90))->q_name);
        }
        f_parser__add_kid(v_t, v_key);
        if (({ const char *tb_s166 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 92))->q_kind; ((unsigned char)tb_s166[0] == 110 && (unsigned char)tb_s166[1] == 101 && (unsigned char)tb_s166[2] == 119 && (unsigned char)tb_s166[3] == 108 && (unsigned char)tb_s166[4] == 105 && (unsigned char)tb_s166[5] == 110 && (unsigned char)tb_s166[6] == 101 && tb_s166[7] == 0); })) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после ключа напиши тип значения: dict text int", "");
        }
        f_parser__add_kid(v_t, f_parser__parse_type(v_c));
        return v_t;
    }
    if (f_parser__at_type_word(v_c)) {
        { const char * tb_v167 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 97))->q_word; const char * *tb_p167 = &(((tb_o_Node *)tb_nn(v_t, 97))->q_name); *tb_p167 = tb_v167; }
        f_parser__next(v_c);
        if (({ const char *tb_s168 = ((tb_o_Node *)tb_nn(v_t, 99))->q_name; ((unsigned char)tb_s168[0] == 108 && (unsigned char)tb_s168[1] == 105 && (unsigned char)tb_s168[2] == 115 && (unsigned char)tb_s168[3] == 116 && tb_s168[4] == 0); })) {
            if (((!f_parser__at_type_word(v_c)) && ({ const char *tb_s169 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 100))->q_kind; !((unsigned char)tb_s169[0] == 110 && (unsigned char)tb_s169[1] == 97 && (unsigned char)tb_s169[2] == 109 && (unsigned char)tb_s169[3] == 101 && tb_s169[4] == 0); }))) {
                f_errors__error_at(v_c, f_parser__line_now(v_c), "после list напиши, что в нём лежит: list int, list text ...", "");
            }
            f_parser__add_kid(v_t, f_parser__parse_type(v_c));
        }
        return v_t;
    }
    if (({ const char *tb_s170 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 104))->q_kind; ((unsigned char)tb_s170[0] == 110 && (unsigned char)tb_s170[1] == 97 && (unsigned char)tb_s170[2] == 109 && (unsigned char)tb_s170[3] == 101 && tb_s170[4] == 0); })) {
        { const char * tb_v171 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 105))->q_word; const char * *tb_p171 = &(((tb_o_Node *)tb_nn(v_t, 105))->q_name); *tb_p171 = tb_v171; }
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
    { const char * tb_v172 = v_sign; const char * *tb_p172 = &(((tb_o_Node *)tb_nn(v_n, 115))->q_value); *tb_p172 = tb_v172; }
    f_parser__add_kid(v_n, v_left);
    f_parser__add_kid(v_n, v_right);
    return v_n;
    return v_result;
}

static tb_o_Node * f_parser__parse_call(tb_o_Comp * v_c, const char * v_name, long long v_line) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_call = f_parser__new_node("call", v_line);
    { const char * tb_v173 = v_name; const char * *tb_p173 = &(((tb_o_Node *)tb_nn(v_call, 122))->q_name); *tb_p173 = tb_v173; }
    f_parser__expect_sign(v_c, "(");
    while ((!f_parser__is_sign(v_c, ")"))) {
        tb_o_Node * v_arg = f_parser__new_node("arg", f_parser__line_now(v_c));
        if ((({ const char *tb_s174 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 127))->q_kind; ((unsigned char)tb_s174[0] == 110 && (unsigned char)tb_s174[1] == 97 && (unsigned char)tb_s174[2] == 109 && (unsigned char)tb_s174[3] == 101 && tb_s174[4] == 0); }) && f_parser__ahead_is_sign(v_c, ":"))) {
            { const char * tb_v175 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 128))->q_word; const char * *tb_p175 = &(((tb_o_Node *)tb_nn(v_arg, 128))->q_name); *tb_p175 = tb_v175; }
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
    if (((({ const char *tb_s176 = ((tb_o_Token *)tb_nn(v_t, 143))->q_kind; ((unsigned char)tb_s176[0] == 110 && (unsigned char)tb_s176[1] == 117 && (unsigned char)tb_s176[2] == 109 && (unsigned char)tb_s176[3] == 98 && (unsigned char)tb_s176[4] == 101 && (unsigned char)tb_s176[5] == 114 && tb_s176[6] == 0); }) || ({ const char *tb_s177 = ((tb_o_Token *)tb_nn(v_t, 143))->q_kind; ((unsigned char)tb_s177[0] == 100 && (unsigned char)tb_s177[1] == 101 && (unsigned char)tb_s177[2] == 99 && (unsigned char)tb_s177[3] == 105 && (unsigned char)tb_s177[4] == 109 && (unsigned char)tb_s177[5] == 97 && (unsigned char)tb_s177[6] == 108 && tb_s177[7] == 0); })) || ({ const char *tb_s178 = ((tb_o_Token *)tb_nn(v_t, 143))->q_kind; ((unsigned char)tb_s178[0] == 115 && (unsigned char)tb_s178[1] == 116 && (unsigned char)tb_s178[2] == 114 && (unsigned char)tb_s178[3] == 105 && (unsigned char)tb_s178[4] == 110 && (unsigned char)tb_s178[5] == 103 && tb_s178[6] == 0); }))) {
        v_n = f_parser__new_node(((tb_o_Token *)tb_nn(v_t, 144))->q_kind, v_line);
        { const char * tb_v179 = ((tb_o_Token *)tb_nn(v_t, 145))->q_word; const char * *tb_p179 = &(((tb_o_Node *)tb_nn(v_n, 145))->q_value); *tb_p179 = tb_v179; }
        f_parser__next(v_c);
    } else if ((f_parser__is_cmd(v_c, "true") || f_parser__is_cmd(v_c, "false"))) {
        v_n = f_parser__new_node("bool", v_line);
        { const char * tb_v180 = ((tb_o_Token *)tb_nn(v_t, 149))->q_word; const char * *tb_p180 = &(((tb_o_Node *)tb_nn(v_n, 149))->q_value); *tb_p180 = tb_v180; }
        f_parser__next(v_c);
    } else if (({ const char *tb_s181 = ((tb_o_Token *)tb_nn(v_t, 151))->q_kind; ((unsigned char)tb_s181[0] == 110 && (unsigned char)tb_s181[1] == 97 && (unsigned char)tb_s181[2] == 109 && (unsigned char)tb_s181[3] == 101 && tb_s181[4] == 0); })) {
        f_parser__next(v_c);
        if (f_parser__is_sign(v_c, "(")) {
            v_n = f_parser__parse_call(v_c, ((tb_o_Token *)tb_nn(v_t, 154))->q_word, v_line);
        } else {
            v_n = f_parser__new_node("name", v_line);
            { const char * tb_v182 = ((tb_o_Token *)tb_nn(v_t, 157))->q_word; const char * *tb_p182 = &(((tb_o_Node *)tb_nn(v_n, 157))->q_name); *tb_p182 = tb_v182; }
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
                { const char * tb_v183 = v_name; const char * *tb_p183 = &(((tb_o_Node *)tb_nn(v_m, 201))->q_name); *tb_p183 = tb_v183; }
                f_parser__add_kid(v_m, v_n);
                {
                    tb_list * tb_l184 = ((tb_o_Node *)tb_nn(v_call, 203))->q_kids;
                    for (long long tb_i184 = 0; tb_i184 < tb_l184->len; tb_i184++) {
                        tb_o_Node * const v_k = ((tb_o_Node * *)tb_l184->data)[tb_i184];
                        f_parser__add_kid(v_m, v_k);
                    }
                }
                v_n = v_m;
            } else {
                tb_o_Node * v_f = f_parser__new_node("field", v_dot_line);
                { const char * tb_v185 = v_name; const char * *tb_p185 = &(((tb_o_Node *)tb_nn(v_f, 208))->q_name); *tb_p185 = tb_v185; }
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
        { const char * tb_v186 = "-"; const char * *tb_p186 = &(((tb_o_Node *)tb_nn(v_n, 216))->q_value); *tb_p186 = tb_v186; }
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
        { const char * tb_v187 = "not"; const char * *tb_p187 = &(((tb_o_Node *)tb_nn(v_n, 254))->q_value); *tb_p187 = tb_v187; }
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
    if ((({ const char *tb_s188 = ((tb_o_Token *)tb_nn(v_t, 300))->q_kind; ((unsigned char)tb_s188[0] == 110 && (unsigned char)tb_s188[1] == 97 && (unsigned char)tb_s188[2] == 109 && (unsigned char)tb_s188[3] == 101 && tb_s188[4] == 0); }) && ({ const char *tb_s189 = ((tb_o_Token *)tb_nn(v_t, 300))->q_word; ((unsigned char)tb_s189[0] == 111 && (unsigned char)tb_s189[1] == 110 && tb_s189[2] == 0); }))) {
        f_parser__next(v_c);
        tb_o_Token * v_a = f_parser__now(v_c);
        tb_o_Token * v_b = f_parser__ahead(v_c);
        if ((({ const char *tb_s190 = ((tb_o_Token *)tb_nn(v_a, 304))->q_word; !((unsigned char)tb_s190[0] == 97 && (unsigned char)tb_s190[1] == 108 && (unsigned char)tb_s190[2] == 108 && tb_s190[3] == 0); }) || ({ const char *tb_s191 = ((tb_o_Token *)tb_nn(v_b, 304))->q_word; !((unsigned char)tb_s191[0] == 99 && (unsigned char)tb_s191[1] == 111 && (unsigned char)tb_s191[2] == 114 && (unsigned char)tb_s191[3] == 101 && (unsigned char)tb_s191[4] == 115 && tb_s191[5] == 0); }))) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после on пишется all cores: each x in nums on all cores", "");
        }
        f_parser__next(v_c);
        f_parser__next(v_c);
        { _Bool tb_v192 = 1; _Bool *tb_p192 = &(((tb_o_Node *)tb_nn(v_n, 308))->q_is_par); *tb_p192 = tb_v192; }
    }
}

static tb_o_Node * f_parser__parse_each(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_n = f_parser__new_node("each", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "each");
    { const char * tb_v193 = f_parser__expect_name(v_c); const char * *tb_p193 = &(((tb_o_Node *)tb_nn(v_n, 313))->q_name); *tb_p193 = tb_v193; }
    f_parser__expect_cmd(v_c, "in");
    f_parser__add_kid(v_n, f_parser__parse_expr(v_c));
    f_parser__parse_on_all_cores(v_c, v_n);
    if (f_parser__is_cmd(v_c, "by")) {
        f_parser__next(v_c);
        if ((({ const char *tb_s194 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 321))->q_kind; ((unsigned char)tb_s194[0] == 110 && (unsigned char)tb_s194[1] == 97 && (unsigned char)tb_s194[2] == 109 && (unsigned char)tb_s194[3] == 101 && tb_s194[4] == 0); }) && ({ const char *tb_s195 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 321))->q_word; ((unsigned char)tb_s195[0] == 118 && (unsigned char)tb_s195[1] == 101 && (unsigned char)tb_s195[2] == 99 && tb_s195[3] == 0); }))) {
            f_parser__next(v_c);
            { _Bool tb_v196 = 1; _Bool *tb_p196 = &(((tb_o_Node *)tb_nn(v_n, 323))->q_by_vec); *tb_p196 = tb_v196; }
            if (((tb_o_Node *)tb_nn(v_n, 324))->q_is_par) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 325))->q_line, "on all cores и by vec вместе пока нельзя", "");
            }
        } else {
            { tb_o_Node * tb_v197 = f_parser__parse_expr(v_c); tb_o_Node * *tb_p197 = &(((tb_o_Node *)tb_nn(v_n, 328))->q_piece_size); *tb_p197 = tb_v197; }
            { _Bool tb_v198 = 1; _Bool *tb_p198 = &(((tb_o_Node *)tb_nn(v_n, 329))->q_by_piece); *tb_p198 = tb_v198; }
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
    { const char * tb_v199 = f_parser__expect_name(v_c); const char * *tb_p199 = &(((tb_o_Node *)tb_nn(v_n, 339))->q_name); *tb_p199 = tb_v199; }
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
        if ((({ const char *tb_s200 = ((tb_o_Node *)tb_nn(v_call, 371))->q_kind; !((unsigned char)tb_s200[0] == 99 && (unsigned char)tb_s200[1] == 97 && (unsigned char)tb_s200[2] == 108 && (unsigned char)tb_s200[3] == 108 && tb_s200[4] == 0); }) && ({ const char *tb_s201 = ((tb_o_Node *)tb_nn(v_call, 371))->q_kind; !((unsigned char)tb_s201[0] == 109 && (unsigned char)tb_s201[1] == 101 && (unsigned char)tb_s201[2] == 116 && (unsigned char)tb_s201[3] == 104 && (unsigned char)tb_s201[4] == 111 && (unsigned char)tb_s201[5] == 100 && tb_s201[6] == 0); }))) {
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
        { const char * tb_v202 = f_parser__expect_name(v_c); const char * *tb_p202 = &(((tb_o_Node *)tb_nn(v_e, 384))->q_name); *tb_p202 = tb_v202; }
        f_parser__expect_newline(v_c);
        f_parser__add_kid(v_e, v_v);
        f_parser__add_kid(v_e, f_parser__parse_block(v_c));
        return v_e;
    }
    return v_v;
    return v_result;
}

static void f_parser__end_statement(tb_o_Comp * v_c, tb_o_Node * v_value) {
    if (({ const char *tb_s203 = ((tb_o_Node *)tb_nn(v_value, 393))->q_kind; !((unsigned char)tb_s203[0] == 101 && (unsigned char)tb_s203[1] == 108 && (unsigned char)tb_s203[2] == 115 && (unsigned char)tb_s203[3] == 101 && (unsigned char)tb_s203[4] == 95 && (unsigned char)tb_s203[5] == 101 && (unsigned char)tb_s203[6] == 114 && (unsigned char)tb_s203[7] == 114 && tb_s203[8] == 0); })) {
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
    { const char * tb_v204 = f_parser__expect_name(v_c); const char * *tb_p204 = &(((tb_o_Node *)tb_nn(v_n, 403))->q_name); *tb_p204 = tb_v204; }
    { _Bool tb_v205 = v_is_const; _Bool *tb_p205 = &(((tb_o_Node *)tb_nn(v_n, 404))->q_is_const); *tb_p205 = tb_v205; }
    f_parser__expect_sign(v_c, "=");
    if ((f_parser__at_type_word(v_c) || f_parser__is_cmd(v_c, "dict"))) {
        { tb_o_Node * tb_v206 = f_parser__parse_type(v_c); tb_o_Node * *tb_p206 = &(((tb_o_Node *)tb_nn(v_n, 407))->q_tsyn); *tb_p206 = tb_v206; }
        { _Bool tb_v207 = 1; _Bool *tb_p207 = &(((tb_o_Node *)tb_nn(v_n, 408))->q_has_tsyn); *tb_p207 = tb_v207; }
    }
    if (({ const char *tb_s208 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 409))->q_kind; !((unsigned char)tb_s208[0] == 110 && (unsigned char)tb_s208[1] == 101 && (unsigned char)tb_s208[2] == 119 && (unsigned char)tb_s208[3] == 108 && (unsigned char)tb_s208[4] == 105 && (unsigned char)tb_s208[5] == 110 && (unsigned char)tb_s208[6] == 101 && tb_s208[7] == 0); })) {
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
    { const char * tb_v209 = f_parser__expect_name(v_c); const char * *tb_p209 = &(((tb_o_Node *)tb_nn(v_n, 424))->q_name); *tb_p209 = tb_v209; }
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
        { const char * tb_v210 = f_parser__expect_name(v_c); const char * *tb_p210 = &(((tb_o_Node *)tb_nn(v_name, 438))->q_name); *tb_p210 = tb_v210; }
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
        if (((({ const char *tb_s211 = v_k; !((unsigned char)tb_s211[0] == 110 && (unsigned char)tb_s211[1] == 97 && (unsigned char)tb_s211[2] == 109 && (unsigned char)tb_s211[3] == 101 && tb_s211[4] == 0); }) && ({ const char *tb_s212 = v_k; !((unsigned char)tb_s212[0] == 105 && (unsigned char)tb_s212[1] == 110 && (unsigned char)tb_s212[2] == 100 && (unsigned char)tb_s212[3] == 101 && (unsigned char)tb_s212[4] == 120 && tb_s212[5] == 0); })) && ({ const char *tb_s213 = v_k; !((unsigned char)tb_s213[0] == 102 && (unsigned char)tb_s213[1] == 105 && (unsigned char)tb_s213[2] == 101 && (unsigned char)tb_s213[3] == 108 && (unsigned char)tb_s213[4] == 100 && tb_s213[5] == 0); }))) {
            f_errors__error_at(v_c, v_line, "слева от знака должно быть имя, nums[i] или u.age", "");
        }
        tb_o_Node * v_n = f_parser__new_node("set", v_line);
        { const char * tb_v214 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 464))->q_word; const char * *tb_p214 = &(((tb_o_Node *)tb_nn(v_n, 464))->q_value); *tb_p214 = tb_v214; }
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
        { const char * tb_v215 = f_parser__expect_name(v_c); const char * *tb_p215 = &(((tb_o_Node *)tb_nn(v_e, 475))->q_name); *tb_p215 = tb_v215; }
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
        tb_list * tb_l216 = tb_list_from(4, sizeof(const char *), (const char *[]){"gives", "obj", "danger", "by"});
        for (long long tb_i216 = 0; tb_i216 < tb_l216->len; tb_i216++) {
            const char * const v_w = ((const char * *)tb_l216->data)[tb_i216];
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
        if (({ const char *tb_s217 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 509))->q_kind; !((unsigned char)tb_s217[0] == 110 && (unsigned char)tb_s217[1] == 101 && (unsigned char)tb_s217[2] == 119 && (unsigned char)tb_s217[3] == 108 && (unsigned char)tb_s217[4] == 105 && (unsigned char)tb_s217[5] == 110 && (unsigned char)tb_s217[6] == 101 && tb_s217[7] == 0); })) {
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
        if ((({ const char *tb_s218 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 547))->q_kind; !((unsigned char)tb_s218[0] == 110 && (unsigned char)tb_s218[1] == 97 && (unsigned char)tb_s218[2] == 109 && (unsigned char)tb_s218[3] == 101 && tb_s218[4] == 0); }) || (!f_parser__ahead_is_sign(v_c, "=")))) {
            f_errors__error_at(v_c, v_line, "после shared пишется новая переменная: shared y = int 0", "");
        }
        tb_o_Node * v_n = f_parser__parse_assign(v_c, 0);
        { _Bool tb_v219 = 1; _Bool *tb_p219 = &(((tb_o_Node *)tb_nn(v_n, 550))->q_is_shared); *tb_p219 = tb_v219; }
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
    if (({ const char *tb_s220 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 562))->q_kind; ((unsigned char)tb_s220[0] == 110 && (unsigned char)tb_s220[1] == 97 && (unsigned char)tb_s220[2] == 109 && (unsigned char)tb_s220[3] == 101 && tb_s220[4] == 0); })) {
        return f_parser__parse_name_statement(v_c);
    }
    if ((({ const char *tb_s221 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 564))->q_kind; ((unsigned char)tb_s221[0] == 99 && (unsigned char)tb_s221[1] == 111 && (unsigned char)tb_s221[2] == 109 && (unsigned char)tb_s221[3] == 109 && (unsigned char)tb_s221[4] == 97 && (unsigned char)tb_s221[5] == 110 && (unsigned char)tb_s221[6] == 100 && tb_s221[7] == 0); }) && f_parser__ahead_is_sign(v_c, "="))) {
        f_errors__error_at(v_c, v_line, "это слово занято языком, возьми другое имя", ((tb_o_Token *)tb_nn(f_parser__now(v_c), 565))->q_word);
    }
    f_errors__error_at(v_c, v_line, "не понимаю, что делает строка, начиная с", f_parser__what_is_here(v_c));
    return f_parser__new_node("missing", v_line);
    return v_result;
}

static tb_o_Node * f_parser__parse_block(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_block = f_parser__new_node("block", f_parser__line_now(v_c));
    if (({ const char *tb_s222 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 571))->q_kind; !((unsigned char)tb_s222[0] == 105 && (unsigned char)tb_s222[1] == 110 && (unsigned char)tb_s222[2] == 100 && (unsigned char)tb_s222[3] == 101 && (unsigned char)tb_s222[4] == 110 && (unsigned char)tb_s222[5] == 116 && tb_s222[6] == 0); })) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "после этой строки нужен блок с отступом", "");
    }
    f_parser__next(v_c);
    while ((({ const char *tb_s223 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 574))->q_kind; !((unsigned char)tb_s223[0] == 100 && (unsigned char)tb_s223[1] == 101 && (unsigned char)tb_s223[2] == 100 && (unsigned char)tb_s223[3] == 101 && (unsigned char)tb_s223[4] == 110 && (unsigned char)tb_s223[5] == 116 && tb_s223[6] == 0); }) && ({ const char *tb_s224 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 574))->q_kind; !((unsigned char)tb_s224[0] == 101 && (unsigned char)tb_s224[1] == 110 && (unsigned char)tb_s224[2] == 100 && tb_s224[3] == 0); }))) {
        if (({ const char *tb_s225 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 575))->q_kind; ((unsigned char)tb_s225[0] == 105 && (unsigned char)tb_s225[1] == 110 && (unsigned char)tb_s225[2] == 100 && (unsigned char)tb_s225[3] == 101 && (unsigned char)tb_s225[4] == 110 && (unsigned char)tb_s225[5] == 116 && tb_s225[6] == 0); })) {
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
        { _Bool tb_v226 = 1; _Bool *tb_p226 = &(((tb_o_Node *)tb_nn(v_p, 588))->q_is_link); *tb_p226 = tb_v226; }
        { const char * tb_v227 = f_parser__expect_name(v_c); const char * *tb_p227 = &(((tb_o_Node *)tb_nn(v_p, 589))->q_name); *tb_p227 = tb_v227; }
        return v_p;
    }
    { const char * tb_v228 = f_parser__expect_name(v_c); const char * *tb_p228 = &(((tb_o_Node *)tb_nn(v_p, 591))->q_name); *tb_p228 = tb_v228; }
    f_parser__expect_sign(v_c, ":");
    { tb_o_Node * tb_v229 = f_parser__parse_type(v_c); tb_o_Node * *tb_p229 = &(((tb_o_Node *)tb_nn(v_p, 593))->q_tsyn); *tb_p229 = tb_v229; }
    { _Bool tb_v230 = 1; _Bool *tb_p230 = &(((tb_o_Node *)tb_nn(v_p, 594))->q_has_tsyn); *tb_p230 = tb_v230; }
    return v_p;
    return v_result;
}

static tb_o_Node * f_parser__parse_func(tb_o_Comp * v_c) {
    tb_o_Node * v_result = 0;
    tb_o_Node * v_f = f_parser__new_node("func", f_parser__line_now(v_c));
    f_parser__expect_cmd(v_c, "func");
    { const char * tb_v231 = f_parser__expect_name(v_c); const char * *tb_p231 = &(((tb_o_Node *)tb_nn(v_f, 601))->q_name); *tb_p231 = tb_v231; }
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
    { const char * tb_v232 = f_parser__expect_name(v_c); const char * *tb_p232 = &(((tb_o_Node *)tb_nn(v_o, 630))->q_name); *tb_p232 = tb_v232; }
    if (f_parser__is_cmd(v_c, "by")) {
        f_parser__next(v_c);
        if ((({ const char *tb_s233 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 634))->q_kind; !((unsigned char)tb_s233[0] == 110 && (unsigned char)tb_s233[1] == 97 && (unsigned char)tb_s233[2] == 109 && (unsigned char)tb_s233[3] == 101 && tb_s233[4] == 0); }) || ({ const char *tb_s234 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 634))->q_word; !((unsigned char)tb_s234[0] == 99 && (unsigned char)tb_s234[1] == 111 && (unsigned char)tb_s234[2] == 108 && (unsigned char)tb_s234[3] == 117 && (unsigned char)tb_s234[4] == 109 && (unsigned char)tb_s234[5] == 110 && (unsigned char)tb_s234[6] == 115 && tb_s234[7] == 0); }))) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "после obj ... by пишется columns: obj Point by columns", "");
        }
        f_parser__next(v_c);
        { _Bool tb_v235 = 1; _Bool *tb_p235 = &(((tb_o_Node *)tb_nn(v_o, 637))->q_by_cols); *tb_p235 = tb_v235; }
    }
    f_parser__expect_newline(v_c);
    if (({ const char *tb_s236 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 639))->q_kind; !((unsigned char)tb_s236[0] == 105 && (unsigned char)tb_s236[1] == 110 && (unsigned char)tb_s236[2] == 100 && (unsigned char)tb_s236[3] == 101 && (unsigned char)tb_s236[4] == 110 && (unsigned char)tb_s236[5] == 116 && tb_s236[6] == 0); })) {
        f_errors__error_at(v_c, f_parser__line_now(v_c), "у obj ниже с отступом пишутся качества: name: text", "");
    }
    f_parser__next(v_c);
    while ((({ const char *tb_s237 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 642))->q_kind; !((unsigned char)tb_s237[0] == 100 && (unsigned char)tb_s237[1] == 101 && (unsigned char)tb_s237[2] == 100 && (unsigned char)tb_s237[3] == 101 && (unsigned char)tb_s237[4] == 110 && (unsigned char)tb_s237[5] == 116 && tb_s237[6] == 0); }) && ({ const char *tb_s238 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 642))->q_kind; !((unsigned char)tb_s238[0] == 101 && (unsigned char)tb_s238[1] == 110 && (unsigned char)tb_s238[2] == 100 && tb_s238[3] == 0); }))) {
        f_parser__add_kid(v_o, f_parser__parse_param(v_c, 0));
        f_parser__expect_newline(v_c);
    }
    f_parser__next(v_c);
    return v_o;
    return v_result;
}

static tb_o_Node * f_parser__parse(tb_o_Comp * v_c) {
    { long long tb_v239 = 0LL; long long *tb_p239 = &(((tb_o_Comp *)tb_nn(v_c, 649))->q_pos); *tb_p239 = tb_v239; }
    tb_o_Node * v_program = f_parser__new_node("program", 1LL);
    while (({ const char *tb_s240 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 651))->q_kind; !((unsigned char)tb_s240[0] == 101 && (unsigned char)tb_s240[1] == 110 && (unsigned char)tb_s240[2] == 100 && tb_s240[3] == 0); })) {
        if (f_parser__is_cmd(v_c, "use")) {
            tb_o_Node * v_u = f_parser__new_node("use", f_parser__line_now(v_c));
            f_parser__next(v_c);
            { const char * tb_v241 = f_parser__expect_name(v_c); const char * *tb_p241 = &(((tb_o_Node *)tb_nn(v_u, 656))->q_name); *tb_p241 = tb_v241; }
            f_parser__expect_newline(v_c);
            f_parser__add_kid(v_program, v_u);
        } else if ((f_parser__is_cmd(v_c, "modpub") || f_parser__is_cmd(v_c, "modpri"))) {
            _Bool v_is_pub = f_parser__is_cmd(v_c, "modpub");
            f_parser__next(v_c);
            if ((!f_parser__is_cmd(v_c, "func"))) {
                f_errors__error_at(v_c, f_parser__line_now(v_c), "modpub и modpri ставятся перед func", "");
            }
            tb_o_Node * v_f = f_parser__parse_func(v_c);
            { _Bool tb_v242 = v_is_pub; _Bool *tb_p242 = &(((tb_o_Node *)tb_nn(v_f, 665))->q_is_pub); *tb_p242 = tb_v242; }
            f_parser__add_kid(v_program, v_f);
        } else if (f_parser__is_cmd(v_c, "func")) {
            f_parser__add_kid(v_program, f_parser__parse_func(v_c));
        } else if (f_parser__is_cmd(v_c, "obj")) {
            f_parser__add_kid(v_program, f_parser__parse_obj(v_c));
        } else if (f_parser__is_cmd(v_c, "link")) {
            f_errors__error_at(v_c, f_parser__line_now(v_c), "подключение C-файлов через link будет в версии 0.9 (вместе с danger); файлы Tachbit подключает use", "");
        } else if (({ const char *tb_s243 = ((tb_o_Token *)tb_nn(f_parser__now(v_c), 673))->q_kind; ((unsigned char)tb_s243[0] == 105 && (unsigned char)tb_s243[1] == 110 && (unsigned char)tb_s243[2] == 100 && (unsigned char)tb_s243[3] == 101 && (unsigned char)tb_s243[4] == 110 && (unsigned char)tb_s243[5] == 116 && tb_s243[6] == 0); })) {
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
        long long tb_end244 = v_depth;
        for (long long v_i = 1LL; v_i <= tb_end244; v_i++) {
            tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, "  ", sizeof("  ") - 1); 
        }
    }
    tb_app(&v_s, &tb_rl_s, &tb_rc_s, f_parser__node_kind_name(((tb_o_Node *)tb_nn(v_n, 689))->q_kind)); 
    if (({ const char *tb_s245 = ((tb_o_Node *)tb_nn(v_n, 690))->q_name; !(tb_s245[0] == 0); })) {
        tb_app_n(&v_s, &tb_rl_s, &tb_rc_s, " ", sizeof(" ") - 1); tb_app(&v_s, &tb_rl_s, &tb_rc_s, ((tb_o_Node *)tb_nn(v_n, 691))->q_name); 
    }
    if (({ const char *tb_s246 = ((tb_o_Node *)tb_nn(v_n, 692))->q_value; !(tb_s246[0] == 0); })) {
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
    { const char * tb_v247 = (tb_rl_s = -1, v_s); *(const char * *)tb_grow(v_out) = tb_v247; }
    if (((tb_o_Node *)tb_nn(v_n, 707))->q_has_tsyn) {
        f_parser__tree_lines(((tb_o_Node *)tb_nn(v_n, 708))->q_tsyn, (v_depth + 1LL), v_out);
    }
    {
        tb_list * tb_l248 = ((tb_o_Node *)tb_nn(v_n, 709))->q_kids;
        for (long long tb_i248 = 0; tb_i248 < tb_l248->len; tb_i248++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l248->data)[tb_i248];
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
        v_func_key = ({ const char *tb_c249_0 = ((tb_o_Node *)tb_nn(v_fnode, 17))->q_mod; long long tb_c249_0n = (long long)strlen(tb_c249_0); const char *tb_c249_1 = "."; long long tb_c249_1n = sizeof(".") - 1; const char *tb_c249_2 = ((tb_o_Node *)tb_nn(v_fnode, 17))->q_name; long long tb_c249_2n = (long long)strlen(tb_c249_2); char *tb_c249r = tb_mem(tb_c249_0n + tb_c249_1n + tb_c249_2n + 1), *tb_c249rw = tb_c249r; memcpy(tb_c249rw, tb_c249_0, tb_c249_0n); tb_c249rw += tb_c249_0n; memcpy(tb_c249rw, tb_c249_1, tb_c249_1n); tb_c249rw += tb_c249_1n; memcpy(tb_c249rw, tb_c249_2, tb_c249_2n); tb_c249rw += tb_c249_2n; *tb_c249rw = 0; (const char *)tb_c249r; });
    }
    {
        tb_list * tb_l250 = ((tb_o_Comp *)tb_nn(v_c, 18))->q_types;
        for (long long tb_i250 = 0; tb_i250 < tb_l250->len; tb_i250++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l250->data)[tb_i250];
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
        { tb_o_Node * tb_v251 = v_onode; tb_o_Node * *tb_p251 = &(((tb_o_Type *)tb_nn(v_t, 25))->q_obj_node); *tb_p251 = tb_v251; }
    }
    if (v_has_func) {
        { tb_o_Node * tb_v252 = v_fnode; tb_o_Node * *tb_p252 = &(((tb_o_Type *)tb_nn(v_t, 27))->q_func_node); *tb_p252 = tb_v252; }
    }
    { tb_o_Type * tb_v253 = v_t; *(tb_o_Type * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 28))->q_types) = tb_v253; }
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
    if (({ const char *tb_s254 = ((tb_o_Type *)tb_nn(v_ty, 65))->q_kind; ((unsigned char)tb_s254[0] == 108 && (unsigned char)tb_s254[1] == 105 && (unsigned char)tb_s254[2] == 115 && (unsigned char)tb_s254[3] == 116 && tb_s254[4] == 0); })) {
        return ({ const char *tb_c255_0 = "list "; long long tb_c255_0n = sizeof("list ") - 1; const char *tb_c255_1 = f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 66))->q_elem); long long tb_c255_1n = (long long)strlen(tb_c255_1); char *tb_c255r = tb_mem(tb_c255_0n + tb_c255_1n + 1), *tb_c255rw = tb_c255r; memcpy(tb_c255rw, tb_c255_0, tb_c255_0n); tb_c255rw += tb_c255_0n; memcpy(tb_c255rw, tb_c255_1, tb_c255_1n); tb_c255rw += tb_c255_1n; *tb_c255rw = 0; (const char *)tb_c255r; });
    }
    if (({ const char *tb_s256 = ((tb_o_Type *)tb_nn(v_ty, 67))->q_kind; ((unsigned char)tb_s256[0] == 100 && (unsigned char)tb_s256[1] == 105 && (unsigned char)tb_s256[2] == 99 && (unsigned char)tb_s256[3] == 116 && tb_s256[4] == 0); })) {
        return ({ const char *tb_c257_0 = "dict "; long long tb_c257_0n = sizeof("dict ") - 1; const char *tb_c257_1 = f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 68))->q_key); long long tb_c257_1n = (long long)strlen(tb_c257_1); const char *tb_c257_2 = " "; long long tb_c257_2n = sizeof(" ") - 1; const char *tb_c257_3 = f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_ty, 68))->q_elem); long long tb_c257_3n = (long long)strlen(tb_c257_3); char *tb_c257r = tb_mem(tb_c257_0n + tb_c257_1n + tb_c257_2n + tb_c257_3n + 1), *tb_c257rw = tb_c257r; memcpy(tb_c257rw, tb_c257_0, tb_c257_0n); tb_c257rw += tb_c257_0n; memcpy(tb_c257rw, tb_c257_1, tb_c257_1n); tb_c257rw += tb_c257_1n; memcpy(tb_c257rw, tb_c257_2, tb_c257_2n); tb_c257rw += tb_c257_2n; memcpy(tb_c257rw, tb_c257_3, tb_c257_3n); tb_c257rw += tb_c257_3n; *tb_c257rw = 0; (const char *)tb_c257r; });
    }
    if (({ const char *tb_s258 = ((tb_o_Type *)tb_nn(v_ty, 69))->q_kind; ((unsigned char)tb_s258[0] == 111 && (unsigned char)tb_s258[1] == 98 && (unsigned char)tb_s258[2] == 106 && tb_s258[3] == 0); })) {
        return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn(v_ty, 70))->q_obj_node, 70))->q_name;
    }
    if (({ const char *tb_s259 = ((tb_o_Type *)tb_nn(v_ty, 71))->q_kind; ((unsigned char)tb_s259[0] == 109 && (unsigned char)tb_s259[1] == 117 && (unsigned char)tb_s259[2] == 108 && (unsigned char)tb_s259[3] == 116 && (unsigned char)tb_s259[4] == 105 && tb_s259[5] == 0); })) {
        return "несколько выходов";
    }
    if (({ const char *tb_s260 = ((tb_o_Type *)tb_nn(v_ty, 73))->q_kind; ((unsigned char)tb_s260[0] == 110 && (unsigned char)tb_s260[1] == 111 && (unsigned char)tb_s260[2] == 110 && (unsigned char)tb_s260[3] == 101 && tb_s260[4] == 0); })) {
        return "ничего";
    }
    if (({ const char *tb_s261 = ((tb_o_Type *)tb_nn(v_ty, 75))->q_kind; ((unsigned char)tb_s261[0] == 116 && (unsigned char)tb_s261[1] == 97 && (unsigned char)tb_s261[2] == 115 && (unsigned char)tb_s261[3] == 107 && tb_s261[4] == 0); })) {
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
    return ({ const char *tb_s262 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s262[0] == 108 && (unsigned char)tb_s262[1] == 105 && (unsigned char)tb_s262[2] == 115 && (unsigned char)tb_s262[3] == 116 && tb_s262[4] == 0); });
    tb_crash(82, "функция is_list дошла до конца без return");
    return 0;
}

static _Bool f_check__is_dict(tb_o_Comp * v_c, long long v_t) {
    return ({ const char *tb_s263 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s263[0] == 100 && (unsigned char)tb_s263[1] == 105 && (unsigned char)tb_s263[2] == 99 && (unsigned char)tb_s263[3] == 116 && tb_s263[4] == 0); });
    tb_crash(85, "функция is_dict дошла до конца без return");
    return 0;
}

static _Bool f_check__is_cols_obj(tb_o_Comp * v_c, long long v_t) {
    return (({ const char *tb_s264 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s264[0] == 111 && (unsigned char)tb_s264[1] == 98 && (unsigned char)tb_s264[2] == 106 && tb_s264[3] == 0); }) && ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 90))->q_types, v_t, sizeof(tb_o_Type *), 90)), 90))->q_obj_node, 90))->q_by_cols);
    tb_crash(89, "функция is_cols_obj дошла до конца без return");
    return 0;
}

static _Bool f_check__is_cols_list(tb_o_Comp * v_c, long long v_t) {
    return (({ const char *tb_s265 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s265[0] == 108 && (unsigned char)tb_s265[1] == 105 && (unsigned char)tb_s265[2] == 115 && (unsigned char)tb_s265[3] == 116 && tb_s265[4] == 0); }) && f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 94))->q_types, v_t, sizeof(tb_o_Type *), 94)), 94))->q_elem));
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
        tb_list * tb_l266 = tb_list_from(29, sizeof(const char *), (const char *[]){"print", "len", "round", "code", "char", "cut", "to_int", "to_dec", "read_file", "write_file", "input", "args", "split", "join", "find", "replace", "upper", "lower", "trim", "starts_with", "ends_with", "sort", "abs", "min", "max", "sqrt", "run", "exit", "file_exists"});
        for (long long tb_i266 = 0; tb_i266 < tb_l266->len; tb_i266++) {
            const char * const v_b = ((const char * *)tb_l266->data)[tb_i266];
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
    return ((((({ const char *tb_s267 = v_name; ((unsigned char)tb_s267[0] == 116 && (unsigned char)tb_s267[1] == 111 && (unsigned char)tb_s267[2] == 95 && (unsigned char)tb_s267[3] == 105 && (unsigned char)tb_s267[4] == 110 && (unsigned char)tb_s267[5] == 116 && tb_s267[6] == 0); }) || ({ const char *tb_s268 = v_name; ((unsigned char)tb_s268[0] == 116 && (unsigned char)tb_s268[1] == 111 && (unsigned char)tb_s268[2] == 95 && (unsigned char)tb_s268[3] == 100 && (unsigned char)tb_s268[4] == 101 && (unsigned char)tb_s268[5] == 99 && tb_s268[6] == 0); })) || (strcmp(v_name, "read_file") == 0)) || (strcmp(v_name, "write_file") == 0)) || ({ const char *tb_s269 = v_name; ((unsigned char)tb_s269[0] == 105 && (unsigned char)tb_s269[1] == 110 && (unsigned char)tb_s269[2] == 112 && (unsigned char)tb_s269[3] == 117 && (unsigned char)tb_s269[4] == 116 && tb_s269[5] == 0); }));
    tb_crash(116, "функция builtin_can_fail дошла до конца без return");
    return 0;
}

static _Bool f_check__is_module(tb_o_Comp * v_c, const char * v_name) {
    {
        tb_list * tb_l270 = ((tb_o_Comp *)tb_nn(v_c, 123))->q_all_mods;
        for (long long tb_i270 = 0; tb_i270 < tb_l270->len; tb_i270++) {
            const char * const v_m = ((const char * *)tb_l270->data)[tb_i270];
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
        tb_list * tb_l271 = ((tb_o_Comp *)tb_nn(v_c, 131))->q_uses;
        for (long long tb_i271 = 0; tb_i271 < tb_l271->len; tb_i271++) {
            tb_o_Use * const v_u = ((tb_o_Use * *)tb_l271->data)[tb_i271];
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
        long long tb_end272 = ((((tb_o_Comp *)tb_nn(v_c, 138))->q_funcs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end272; v_i++) {
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
        long long tb_end273 = ((((tb_o_Comp *)tb_nn(v_c, 150))->q_funcs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end273; v_i++) {
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
        tb_list * tb_l274 = ((tb_o_Comp *)tb_nn(v_c, 160))->q_funcs;
        for (long long tb_i274 = 0; tb_i274 < tb_l274->len; tb_i274++) {
            tb_o_Func * const v_f = ((tb_o_Func * *)tb_l274->data)[tb_i274];
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
        long long tb_end275 = ((((tb_o_Comp *)tb_nn(v_c, 169))->q_objs)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end275; v_i++) {
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
        long long tb_end276 = ((((tb_o_Node *)tb_nn(v_o, 175))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end276; v_i++) {
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
    if (({ const char *tb_s277 = ((tb_o_Node *)tb_nn(v_t, 181))->q_name; ((unsigned char)tb_s277[0] == 105 && (unsigned char)tb_s277[1] == 110 && (unsigned char)tb_s277[2] == 116 && tb_s277[3] == 0); })) {
        return 1LL;
    }
    if (({ const char *tb_s278 = ((tb_o_Node *)tb_nn(v_t, 183))->q_name; ((unsigned char)tb_s278[0] == 100 && (unsigned char)tb_s278[1] == 101 && (unsigned char)tb_s278[2] == 99 && tb_s278[3] == 0); })) {
        return 2LL;
    }
    if (({ const char *tb_s279 = ((tb_o_Node *)tb_nn(v_t, 185))->q_name; ((unsigned char)tb_s279[0] == 116 && (unsigned char)tb_s279[1] == 101 && (unsigned char)tb_s279[2] == 120 && (unsigned char)tb_s279[3] == 116 && tb_s279[4] == 0); })) {
        return 4LL;
    }
    if (({ const char *tb_s280 = ((tb_o_Node *)tb_nn(v_t, 187))->q_name; ((unsigned char)tb_s280[0] == 98 && (unsigned char)tb_s280[1] == 111 && (unsigned char)tb_s280[2] == 111 && (unsigned char)tb_s280[3] == 108 && tb_s280[4] == 0); })) {
        return 3LL;
    }
    if (({ const char *tb_s281 = ((tb_o_Node *)tb_nn(v_t, 189))->q_name; ((unsigned char)tb_s281[0] == 108 && (unsigned char)tb_s281[1] == 105 && (unsigned char)tb_s281[2] == 115 && (unsigned char)tb_s281[3] == 116 && tb_s281[4] == 0); })) {
        return f_check__list_of(v_c, f_check__resolve_type(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 190))->q_kids, 0LL, sizeof(tb_o_Node *), 190))));
    }
    if (({ const char *tb_s282 = ((tb_o_Node *)tb_nn(v_t, 191))->q_name; ((unsigned char)tb_s282[0] == 100 && (unsigned char)tb_s282[1] == 105 && (unsigned char)tb_s282[2] == 99 && (unsigned char)tb_s282[3] == 116 && tb_s282[4] == 0); })) {
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
        { long long tb_v283 = 1LL; long long *tb_p283 = &(v_i); *tb_p283 -= tb_v283; }
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
    { tb_o_Var * tb_v284 = tb_new_Var(v_name, v_type, v_is_const, v_is_link, ((tb_o_Comp *)tb_nn(v_c, 224))->q_depth, 0, ""); *(tb_o_Var * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 224))->q_vars) = tb_v284; }
}

static void f_check__cut_vars(tb_o_Comp * v_c, long long v_count_left) {
    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();
    while (((((tb_o_Comp *)tb_nn(v_c, 227))->q_vars)->len > v_count_left)) {
        tb_mark tb_am285 = tb_temp_begin();
        do {
            tb_o_Var * v_gone = (*(tb_o_Var * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 228))->q_vars, 228));
        } while (0);
        tb_temp_end(tb_am285);
    }
    if (tb_fa) tb_temp_end(tb_fm);
}

static void f_check__open_scope(tb_o_Comp * v_c) {
    { long long tb_v286 = 1LL; long long *tb_p286 = &(((tb_o_Comp *)tb_nn(v_c, 231))->q_depth); *tb_p286 += tb_v286; }
}

static void f_check__close_scope(tb_o_Comp * v_c) {
    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();
    while ((((((tb_o_Comp *)tb_nn(v_c, 234))->q_vars)->len > 0LL) && (((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 234))->q_vars, ((((tb_o_Comp *)tb_nn(v_c, 234))->q_vars)->len - 1LL), sizeof(tb_o_Var *), 234)), 234))->q_depth == ((tb_o_Comp *)tb_nn(v_c, 234))->q_depth))) {
        tb_mark tb_am287 = tb_temp_begin();
        do {
            tb_o_Var * v_gone = (*(tb_o_Var * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 235))->q_vars, 235));
        } while (0);
        tb_temp_end(tb_am287);
    }
    { long long tb_v288 = 1LL; long long *tb_p288 = &(((tb_o_Comp *)tb_nn(v_c, 236))->q_depth); *tb_p288 -= tb_v288; }
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
    if (((({ const char *tb_s289 = ((tb_o_Node *)tb_nn(v_n, 251))->q_kind; ((unsigned char)tb_s289[0] == 105 && (unsigned char)tb_s289[1] == 110 && (unsigned char)tb_s289[2] == 100 && (unsigned char)tb_s289[3] == 101 && (unsigned char)tb_s289[4] == 120 && tb_s289[5] == 0); }) || ({ const char *tb_s290 = ((tb_o_Node *)tb_nn(v_n, 251))->q_kind; ((unsigned char)tb_s290[0] == 102 && (unsigned char)tb_s290[1] == 105 && (unsigned char)tb_s290[2] == 101 && (unsigned char)tb_s290[3] == 108 && (unsigned char)tb_s290[4] == 100 && tb_s290[5] == 0); })) || ({ const char *tb_s291 = ((tb_o_Node *)tb_nn(v_n, 251))->q_kind; ((unsigned char)tb_s291[0] == 109 && (unsigned char)tb_s291[1] == 101 && (unsigned char)tb_s291[2] == 116 && (unsigned char)tb_s291[3] == 104 && (unsigned char)tb_s291[4] == 111 && (unsigned char)tb_s291[5] == 100 && tb_s291[6] == 0); }))) {
        return f_check__root_var(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 252))->q_kids, 0LL, sizeof(tb_o_Node *), 252)));
    }
    if (({ const char *tb_s292 = ((tb_o_Node *)tb_nn(v_n, 253))->q_kind; ((unsigned char)tb_s292[0] == 110 && (unsigned char)tb_s292[1] == 97 && (unsigned char)tb_s292[2] == 109 && (unsigned char)tb_s292[3] == 101 && tb_s292[4] == 0); })) {
        return f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 254))->q_name);
    }
    return (-1LL);
    return v_r;
}

static void f_check__capture(tb_o_Comp * v_c, long long v_v) {
    tb_o_Var * v_var = (*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 259))->q_vars, v_v, sizeof(tb_o_Var *), 259));
    {
        tb_list * tb_l293 = ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 260))->q_par_node, 260))->q_caps;
        for (long long tb_i293 = 0; tb_i293 < tb_l293->len; tb_i293++) {
            tb_o_Var * const v_old = ((tb_o_Var * *)tb_l293->data)[tb_i293];
            if (((strcmp(((tb_o_Var *)tb_nn(v_old, 261))->q_name, ((tb_o_Var *)tb_nn(v_var, 261))->q_name) == 0) && ((!((tb_o_Comp *)tb_nn(v_c, 261))->q_par_vec) || (!((tb_o_Var *)tb_nn(v_old, 261))->q_is_shared)))) {
                return;
            }
        }
    }
    { tb_o_Var * tb_v294 = tb_new_Var(((tb_o_Var *)tb_nn(v_var, 263))->q_name, ((tb_o_Var *)tb_nn(v_var, 263))->q_type, 0, ((tb_o_Var *)tb_nn(v_var, 263))->q_is_link, 0LL, (((tb_o_Var *)tb_nn(v_var, 263))->q_is_shared && (!((tb_o_Comp *)tb_nn(v_c, 263))->q_par_vec)), ""); *(tb_o_Var * *)tb_grow(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 263))->q_par_node, 263))->q_caps) = tb_v294; }
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
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 288))->q_line, ({ const char *tb_c295_0 = "внутри "; long long tb_c295_0n = sizeof("внутри ") - 1; const char *tb_c295_1 = f_check__loop_word(v_c); long long tb_c295_1n = (long long)strlen(tb_c295_1); const char *tb_c295_2 = " нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)"; long long tb_c295_2n = sizeof(" нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)") - 1; char *tb_c295r = tb_mem(tb_c295_0n + tb_c295_1n + tb_c295_2n + 1), *tb_c295rw = tb_c295r; memcpy(tb_c295rw, tb_c295_0, tb_c295_0n); tb_c295rw += tb_c295_0n; memcpy(tb_c295rw, tb_c295_1, tb_c295_1n); tb_c295rw += tb_c295_1n; memcpy(tb_c295rw, tb_c295_2, tb_c295_2n); tb_c295rw += tb_c295_2n; *tb_c295rw = 0; (const char *)tb_c295r; }), "");
    }
}

static void f_check__need_value(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t) {
    if ((v_t == 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 294))->q_line, "это ничего не возвращает, значение нельзя использовать", ((tb_o_Node *)tb_nn(v_n, 294))->q_name);
    }
    if (({ const char *tb_s296 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s296[0] == 109 && (unsigned char)tb_s296[1] == 117 && (unsigned char)tb_s296[2] == 108 && (unsigned char)tb_s296[3] == 116 && (unsigned char)tb_s296[4] == 105 && tb_s296[5] == 0); })) {
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
    if ((({ const char *tb_s297 = v_op; ((unsigned char)tb_s297[0] == 111 && (unsigned char)tb_s297[1] == 114 && tb_s297[2] == 0); }) && f_check__can_fail_call(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 305))->q_kids, 0LL, sizeof(tb_o_Node *), 305))))) {
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
        { long long tb_v298 = v_a; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 315))->q_temp_types) = tb_v298; }
        { long long tb_v299 = ((((tb_o_Comp *)tb_nn(v_c, 316))->q_temp_types)->len - 1LL); long long *tb_p299 = &(((tb_o_Node *)tb_nn(v_n, 316))->q_temp_no); *tb_p299 = tb_v299; }
        { const char * tb_v300 = "orelse"; const char * *tb_p300 = &(((tb_o_Node *)tb_nn(v_n, 317))->q_value); *tb_p300 = tb_v300; }
        return v_a;
    }
    long long v_a = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 319))->q_kids, 0LL, sizeof(tb_o_Node *), 319)));
    long long v_b = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 320))->q_kids, 1LL, sizeof(tb_o_Node *), 320)));
    if ((({ const char *tb_s301 = v_op; ((unsigned char)tb_s301[0] == 97 && (unsigned char)tb_s301[1] == 110 && (unsigned char)tb_s301[2] == 100 && tb_s301[3] == 0); }) || ({ const char *tb_s302 = v_op; ((unsigned char)tb_s302[0] == 111 && (unsigned char)tb_s302[1] == 114 && tb_s302[2] == 0); }))) {
        if (((v_a != 3LL) || (v_b != 3LL))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 323))->q_line, "and / or работают только с true и false", "");
        }
        return 3LL;
    }
    if ((({ const char *tb_s303 = v_op; ((unsigned char)tb_s303[0] == 61 && (unsigned char)tb_s303[1] == 61 && tb_s303[2] == 0); }) || ({ const char *tb_s304 = v_op; ((unsigned char)tb_s304[0] == 33 && (unsigned char)tb_s304[1] == 61 && tb_s304[2] == 0); }))) {
        if (((!(f_check__is_number(v_a) && f_check__is_number(v_b))) && (v_a != v_b))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 327))->q_line, "нельзя сравнить разные типы", "");
        }
        if ((!f_check__is_scalar(v_a))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 329))->q_line, "списки и объекты пока нельзя сравнивать", "");
        }
        return 3LL;
    }
    if ((((({ const char *tb_s305 = v_op; ((unsigned char)tb_s305[0] == 60 && tb_s305[1] == 0); }) || ({ const char *tb_s306 = v_op; ((unsigned char)tb_s306[0] == 62 && tb_s306[1] == 0); })) || ({ const char *tb_s307 = v_op; ((unsigned char)tb_s307[0] == 60 && (unsigned char)tb_s307[1] == 61 && tb_s307[2] == 0); })) || ({ const char *tb_s308 = v_op; ((unsigned char)tb_s308[0] == 62 && (unsigned char)tb_s308[1] == 61 && tb_s308[2] == 0); }))) {
        if (((v_a == 4LL) && (v_b == 4LL))) {
            return 3LL;
        }
        if (((!f_check__is_number(v_a)) || (!f_check__is_number(v_b)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 335))->q_line, "больше / меньше работают с числами или с двумя текстами", v_op);
        }
        return 3LL;
    }
    if ((({ const char *tb_s309 = v_op; ((unsigned char)tb_s309[0] == 43 && tb_s309[1] == 0); }) && ((v_a == 4LL) || (v_b == 4LL)))) {
        if (((!f_check__is_scalar(v_a)) || (!f_check__is_scalar(v_b)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 339))->q_line, "список или объект нельзя сложить с текстом", "");
        }
        return 4LL;
    }
    if (((!f_check__is_number(v_a)) || (!f_check__is_number(v_b)))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 342))->q_line, "эта операция работает только с числами", v_op);
    }
    if ((({ const char *tb_s310 = v_op; ((unsigned char)tb_s310[0] == 37 && tb_s310[1] == 0); }) && ((v_a == 2LL) || (v_b == 2LL)))) {
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
        long long tb_end311 = ((((tb_o_Node *)tb_nn(v_n, 353))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end311; v_i++) {
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
    if (({ const char *tb_s312 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 365))->q_kids, v_i, sizeof(tb_o_Node *), 365)), 365))->q_name; !(tb_s312[0] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 366))->q_line, "у встроенных функций аргументы пишутся без имён", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 366))->q_kids, v_i, sizeof(tb_o_Node *), 366)), 366))->q_name);
    }
    long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 367))->q_kids, v_i, sizeof(tb_o_Node *), 367)), 367))->q_kids, 0LL, sizeof(tb_o_Node *), 367)));
    { long long tb_v313 = v_t; long long *tb_p313 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 368))->q_kids, v_i, sizeof(tb_o_Node *), 368)), 368))->q_ty); *tb_p313 = tb_v313; }
    return v_t;
    return v_r;
}

static void f_check__builtin_args(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_pattern, const char * v_usage) {
    if (((((tb_o_Node *)tb_nn(v_n, 374))->q_kids)->len != tb_tlen(v_pattern))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 375))->q_line, "не то количество значений, правильно так", v_usage);
    }
    {
        long long tb_end314 = (tb_tlen(v_pattern) - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end314; v_i++) {
            long long v_t = f_check__builtin_arg(v_c, v_n, v_i);
            const char * v_p = tb_char_at(v_pattern, v_i, -1, 378);
            _Bool v_ok = 1;
            if (({ const char *tb_s315 = v_p; ((unsigned char)tb_s315[0] == 116 && tb_s315[1] == 0); })) {
                v_ok = (v_t == 4LL);
            } else if (({ const char *tb_s316 = v_p; ((unsigned char)tb_s316[0] == 105 && tb_s316[1] == 0); })) {
                v_ok = (v_t == 1LL);
            } else if (({ const char *tb_s317 = v_p; ((unsigned char)tb_s317[0] == 110 && tb_s317[1] == 0); })) {
                v_ok = f_check__is_number(v_t);
            } else if (({ const char *tb_s318 = v_p; ((unsigned char)tb_s318[0] == 76 && tb_s318[1] == 0); })) {
                v_ok = (f_check__is_list(v_c, v_t) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 387))->q_types, v_t, sizeof(tb_o_Type *), 387)), 387))->q_elem == 4LL));
            } else if (({ const char *tb_s319 = v_p; ((unsigned char)tb_s319[0] == 115 && tb_s319[1] == 0); })) {
                v_ok = (f_check__is_list(v_c, v_t) && (((((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 1LL) || (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 2LL)) || (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 389))->q_types, v_t, sizeof(tb_o_Type *), 389)), 389))->q_elem == 4LL)));
            } else if (({ const char *tb_s320 = v_p; ((unsigned char)tb_s320[0] == 120 && tb_s320[1] == 0); })) {
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
    if (({ const char *tb_s321 = v_name; ((unsigned char)tb_s321[0] == 112 && (unsigned char)tb_s321[1] == 114 && (unsigned char)tb_s321[2] == 105 && (unsigned char)tb_s321[3] == 110 && (unsigned char)tb_s321[4] == 116 && tb_s321[5] == 0); })) {
        {
            long long tb_end322 = ((((tb_o_Node *)tb_nn(v_n, 398))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end322; v_i++) {
                long long v_t = f_check__builtin_arg(v_c, v_n, v_i);
            }
        }
        return 0LL;
    }
    if (({ const char *tb_s323 = v_name; ((unsigned char)tb_s323[0] == 108 && (unsigned char)tb_s323[1] == 101 && (unsigned char)tb_s323[2] == 110 && tb_s323[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "x", "len(список, текст или dict)");
        return 1LL;
    }
    if (({ const char *tb_s324 = v_name; ((unsigned char)tb_s324[0] == 114 && (unsigned char)tb_s324[1] == 111 && (unsigned char)tb_s324[2] == 117 && (unsigned char)tb_s324[3] == 110 && (unsigned char)tb_s324[4] == 100 && tb_s324[5] == 0); })) {
        f_check__builtin_args(v_c, v_n, "n", "round(число)");
        return 1LL;
    }
    if (({ const char *tb_s325 = v_name; ((unsigned char)tb_s325[0] == 99 && (unsigned char)tb_s325[1] == 111 && (unsigned char)tb_s325[2] == 100 && (unsigned char)tb_s325[3] == 101 && tb_s325[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "code(\"A\")  →  65");
        return 1LL;
    }
    if (({ const char *tb_s326 = v_name; ((unsigned char)tb_s326[0] == 99 && (unsigned char)tb_s326[1] == 104 && (unsigned char)tb_s326[2] == 97 && (unsigned char)tb_s326[3] == 114 && tb_s326[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "i", "char(66)  →  \"B\"");
        return 4LL;
    }
    if (({ const char *tb_s327 = v_name; ((unsigned char)tb_s327[0] == 99 && (unsigned char)tb_s327[1] == 117 && (unsigned char)tb_s327[2] == 116 && tb_s327[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "tii", "cut(текст, с какого номера, по какой номер)");
        return 4LL;
    }
    if (({ const char *tb_s328 = v_name; ((unsigned char)tb_s328[0] == 116 && (unsigned char)tb_s328[1] == 111 && (unsigned char)tb_s328[2] == 95 && (unsigned char)tb_s328[3] == 105 && (unsigned char)tb_s328[4] == 110 && (unsigned char)tb_s328[5] == 116 && tb_s328[6] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "to_int(\"42\")");
        return 1LL;
    }
    if (({ const char *tb_s329 = v_name; ((unsigned char)tb_s329[0] == 116 && (unsigned char)tb_s329[1] == 111 && (unsigned char)tb_s329[2] == 95 && (unsigned char)tb_s329[3] == 100 && (unsigned char)tb_s329[4] == 101 && (unsigned char)tb_s329[5] == 99 && tb_s329[6] == 0); })) {
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
    if (({ const char *tb_s330 = v_name; ((unsigned char)tb_s330[0] == 105 && (unsigned char)tb_s330[1] == 110 && (unsigned char)tb_s330[2] == 112 && (unsigned char)tb_s330[3] == 117 && (unsigned char)tb_s330[4] == 116 && tb_s330[5] == 0); })) {
        f_check__builtin_args(v_c, v_n, "", "input()");
        return 4LL;
    }
    if (({ const char *tb_s331 = v_name; ((unsigned char)tb_s331[0] == 97 && (unsigned char)tb_s331[1] == 114 && (unsigned char)tb_s331[2] == 103 && (unsigned char)tb_s331[3] == 115 && tb_s331[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "", "args()");
        return f_check__list_of(v_c, 4LL);
    }
    if (({ const char *tb_s332 = v_name; ((unsigned char)tb_s332[0] == 115 && (unsigned char)tb_s332[1] == 112 && (unsigned char)tb_s332[2] == 108 && (unsigned char)tb_s332[3] == 105 && (unsigned char)tb_s332[4] == 116 && tb_s332[5] == 0); })) {
        f_check__builtin_args(v_c, v_n, "tt", "split(текст, разделитель)");
        return f_check__list_of(v_c, 4LL);
    }
    if (({ const char *tb_s333 = v_name; ((unsigned char)tb_s333[0] == 106 && (unsigned char)tb_s333[1] == 111 && (unsigned char)tb_s333[2] == 105 && (unsigned char)tb_s333[3] == 110 && tb_s333[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "Lt", "join(список text, разделитель)");
        return 4LL;
    }
    if (({ const char *tb_s334 = v_name; ((unsigned char)tb_s334[0] == 102 && (unsigned char)tb_s334[1] == 105 && (unsigned char)tb_s334[2] == 110 && (unsigned char)tb_s334[3] == 100 && tb_s334[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "tt", "find(текст, что ищем)  →  номер или -1");
        return 1LL;
    }
    if (({ const char *tb_s335 = v_name; ((unsigned char)tb_s335[0] == 114 && (unsigned char)tb_s335[1] == 101 && (unsigned char)tb_s335[2] == 112 && (unsigned char)tb_s335[3] == 108 && (unsigned char)tb_s335[4] == 97 && (unsigned char)tb_s335[5] == 99 && (unsigned char)tb_s335[6] == 101 && tb_s335[7] == 0); })) {
        f_check__builtin_args(v_c, v_n, "ttt", "replace(текст, что, на что)");
        return 4LL;
    }
    if (((({ const char *tb_s336 = v_name; ((unsigned char)tb_s336[0] == 117 && (unsigned char)tb_s336[1] == 112 && (unsigned char)tb_s336[2] == 112 && (unsigned char)tb_s336[3] == 101 && (unsigned char)tb_s336[4] == 114 && tb_s336[5] == 0); }) || ({ const char *tb_s337 = v_name; ((unsigned char)tb_s337[0] == 108 && (unsigned char)tb_s337[1] == 111 && (unsigned char)tb_s337[2] == 119 && (unsigned char)tb_s337[3] == 101 && (unsigned char)tb_s337[4] == 114 && tb_s337[5] == 0); })) || ({ const char *tb_s338 = v_name; ((unsigned char)tb_s338[0] == 116 && (unsigned char)tb_s338[1] == 114 && (unsigned char)tb_s338[2] == 105 && (unsigned char)tb_s338[3] == 109 && tb_s338[4] == 0); }))) {
        f_check__builtin_args(v_c, v_n, "t", "upper(текст)");
        return 4LL;
    }
    if (((strcmp(v_name, "starts_with") == 0) || (strcmp(v_name, "ends_with") == 0))) {
        f_check__builtin_args(v_c, v_n, "tt", "starts_with(текст, начало)");
        return 3LL;
    }
    if (({ const char *tb_s339 = v_name; ((unsigned char)tb_s339[0] == 115 && (unsigned char)tb_s339[1] == 111 && (unsigned char)tb_s339[2] == 114 && (unsigned char)tb_s339[3] == 116 && tb_s339[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "s", "sort(список int, dec или text)");
        return 0LL;
    }
    if (({ const char *tb_s340 = v_name; ((unsigned char)tb_s340[0] == 114 && (unsigned char)tb_s340[1] == 117 && (unsigned char)tb_s340[2] == 110 && tb_s340[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "t", "run(\"команда терминала\")  →  код выхода int");
        return 1LL;
    }
    if (({ const char *tb_s341 = v_name; ((unsigned char)tb_s341[0] == 101 && (unsigned char)tb_s341[1] == 120 && (unsigned char)tb_s341[2] == 105 && (unsigned char)tb_s341[3] == 116 && tb_s341[4] == 0); })) {
        f_check__builtin_args(v_c, v_n, "i", "exit(код)");
        return 0LL;
    }
    if ((strcmp(v_name, "file_exists") == 0)) {
        f_check__builtin_args(v_c, v_n, "t", "file_exists(\"путь\")");
        return 3LL;
    }
    if (({ const char *tb_s342 = v_name; ((unsigned char)tb_s342[0] == 97 && (unsigned char)tb_s342[1] == 98 && (unsigned char)tb_s342[2] == 115 && tb_s342[3] == 0); })) {
        f_check__builtin_args(v_c, v_n, "n", "abs(число)");
        return ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 466))->q_kids, 0LL, sizeof(tb_o_Node *), 466)), 466))->q_ty;
    }
    if (({ const char *tb_s343 = v_name; ((unsigned char)tb_s343[0] == 115 && (unsigned char)tb_s343[1] == 113 && (unsigned char)tb_s343[2] == 114 && (unsigned char)tb_s343[3] == 116 && tb_s343[4] == 0); })) {
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
        long long tb_end344 = v_np;
        for (long long v_j = 1LL; v_j <= tb_end344; v_j++) {
            { tb_o_Node * tb_v345 = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *))); *(tb_o_Node * *)tb_grow(v_ordered) = tb_v345; }
            { _Bool tb_v346 = 0; tb_badd(v_taken, tb_v346); }
        }
    }
    _Bool v_named = 0;
    {
        long long tb_end347 = ((((tb_o_Node *)tb_nn(v_call, 488))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end347; v_i++) {
            tb_o_Node * v_arg = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_call, 489))->q_kids, v_i, sizeof(tb_o_Node *), 489));
            long long v_slot = v_i;
            if (({ const char *tb_s348 = ((tb_o_Node *)tb_nn(v_arg, 491))->q_name; !(tb_s348[0] == 0); })) {
                v_named = 1;
                v_slot = (-1LL);
                {
                    long long tb_end349 = (v_np - 1LL);
                    for (long long v_j = 0LL; v_j <= tb_end349; v_j++) {
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
            { tb_o_Node * tb_v350 = v_arg; tb_o_Node * *tb_p350 = &((*(tb_o_Node * *)tb_at(v_ordered, v_slot, sizeof(tb_o_Node *), 503))); *tb_p350 = tb_v350; }
            { _Bool tb_v351 = 1; tb_bset(v_taken, v_slot, tb_v351, 504); }
        }
    }
    { tb_list * tb_v352 = v_ordered; tb_list * *tb_p352 = &(((tb_o_Node *)tb_nn(v_call, 506))->q_kids); *tb_p352 = tb_v352; }
}

static void f_check__need_place_for(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_why, _Bool v_for_link) {
    if ((({ const char *tb_s353 = ((tb_o_Node *)tb_nn(v_n, 510))->q_kind; ((unsigned char)tb_s353[0] == 105 && (unsigned char)tb_s353[1] == 110 && (unsigned char)tb_s353[2] == 100 && (unsigned char)tb_s353[3] == 101 && (unsigned char)tb_s353[4] == 120 && tb_s353[5] == 0); }) && f_check__is_dict(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 510))->q_kids, 0LL, sizeof(tb_o_Node *), 510)), 510))->q_ty))) {
        if (v_for_link) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 512))->q_line, "ярлык на значение в dict нельзя: dict может переложить значения", "");
        }
        return;
    }
    if (({ const char *tb_s354 = ((tb_o_Node *)tb_nn(v_n, 514))->q_kind; ((unsigned char)tb_s354[0] == 110 && (unsigned char)tb_s354[1] == 97 && (unsigned char)tb_s354[2] == 109 && (unsigned char)tb_s354[3] == 101 && tb_s354[4] == 0); })) {
        long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 515))->q_name);
        if (((v_v >= 0LL) && ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 516))->q_vars, v_v, sizeof(tb_o_Var *), 516)), 516))->q_is_const)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 517))->q_line, "это const или переменная цикла, её нельзя менять", ((tb_o_Node *)tb_nn(v_n, 517))->q_name);
        }
        return;
    }
    if (({ const char *tb_s355 = ((tb_o_Node *)tb_nn(v_n, 519))->q_kind; ((unsigned char)tb_s355[0] == 102 && (unsigned char)tb_s355[1] == 105 && (unsigned char)tb_s355[2] == 101 && (unsigned char)tb_s355[3] == 108 && (unsigned char)tb_s355[4] == 100 && tb_s355[5] == 0); })) {
        return;
    }
    if ((((({ const char *tb_s356 = ((tb_o_Node *)tb_nn(v_n, 521))->q_kind; ((unsigned char)tb_s356[0] == 105 && (unsigned char)tb_s356[1] == 110 && (unsigned char)tb_s356[2] == 100 && (unsigned char)tb_s356[3] == 101 && (unsigned char)tb_s356[4] == 120 && tb_s356[5] == 0); }) && f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 521))->q_kids, 0LL, sizeof(tb_o_Node *), 521)), 521))->q_ty)) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 521))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 521))->q_kids, 0LL, sizeof(tb_o_Node *), 521)), 521))->q_ty, sizeof(tb_o_Type *), 521)), 521))->q_elem == 3LL)) && v_for_link)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 522))->q_line, "ярлык на элемент list bool нельзя: там каждый элемент — один бит, у бита нет своего адреса", "");
    }
    if (((({ const char *tb_s357 = ((tb_o_Node *)tb_nn(v_n, 523))->q_kind; ((unsigned char)tb_s357[0] == 105 && (unsigned char)tb_s357[1] == 110 && (unsigned char)tb_s357[2] == 100 && (unsigned char)tb_s357[3] == 101 && (unsigned char)tb_s357[4] == 120 && tb_s357[5] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 523))->q_kids, 0LL, sizeof(tb_o_Node *), 523)), 523))->q_ty)) && v_for_link)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 524))->q_line, "ярлык на элемент списка колонок нельзя: возьми ярлык на качество, например pts[i].x", "");
    }
    if ((({ const char *tb_s358 = ((tb_o_Node *)tb_nn(v_n, 525))->q_kind; ((unsigned char)tb_s358[0] == 105 && (unsigned char)tb_s358[1] == 110 && (unsigned char)tb_s358[2] == 100 && (unsigned char)tb_s358[3] == 101 && (unsigned char)tb_s358[4] == 120 && tb_s358[5] == 0); }) && f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 525))->q_kids, 0LL, sizeof(tb_o_Node *), 525)), 525))->q_ty))) {
        return;
    }
    if (({ const char *tb_s359 = ((tb_o_Node *)tb_nn(v_n, 527))->q_kind; ((unsigned char)tb_s359[0] == 105 && (unsigned char)tb_s359[1] == 110 && (unsigned char)tb_s359[2] == 100 && (unsigned char)tb_s359[3] == 101 && (unsigned char)tb_s359[4] == 120 && tb_s359[5] == 0); })) {
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
        long long tb_end360 = ((((tb_o_Node *)tb_nn(v_n, 536))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end360; v_i++) {
            if (({ const char *tb_s361 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 537))->q_kids, v_i, sizeof(tb_o_Node *), 537)), 537))->q_kind; ((unsigned char)tb_s361[0] == 109 && (unsigned char)tb_s361[1] == 105 && (unsigned char)tb_s361[2] == 115 && (unsigned char)tb_s361[3] == 115 && (unsigned char)tb_s361[4] == 105 && (unsigned char)tb_s361[5] == 110 && (unsigned char)tb_s361[6] == 103 && tb_s361[7] == 0); })) {
                continue;
            }
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 539))->q_kids, v_i, sizeof(tb_o_Node *), 539)), 539))->q_kids, 0LL, sizeof(tb_o_Node *), 539));
            long long v_t = f_check__check_value(v_c, v_value);
            if ((!f_check__fits(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 541))->q_kids, v_i, sizeof(tb_o_Node *), 541)), 541))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 542))->q_line, "качество другого типа", ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 542))->q_kids, v_i, sizeof(tb_o_Node *), 542)), 542))->q_name);
            }
            { long long tb_v362 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 543))->q_kids, v_i, sizeof(tb_o_Node *), 543)), 543))->q_ty; long long *tb_p362 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 543))->q_kids, v_i, sizeof(tb_o_Node *), 543)), 543))->q_ty); *tb_p362 = tb_v362; }
        }
    }
    { tb_o_Node * tb_v363 = v_o; tb_o_Node * *tb_p363 = &(((tb_o_Node *)tb_nn(v_n, 544))->q_ref); *tb_p363 = tb_v363; }
    { _Bool tb_v364 = 1; _Bool *tb_p364 = &(((tb_o_Node *)tb_nn(v_n, 545))->q_has_ref); *tb_p364 = tb_v364; }
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
    if (({ const char *tb_s365 = ((tb_o_Node *)tb_nn(v_n, 566))->q_name; ((unsigned char)tb_s365[0] == 109 && (unsigned char)tb_s365[1] == 97 && (unsigned char)tb_s365[2] == 105 && (unsigned char)tb_s365[3] == 110 && tb_s365[4] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 567))->q_line, "main запускается сама, её нельзя вызывать", "");
    }
    if ((((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 568))->q_node, 568))->q_can_fail && (!((tb_o_Node *)tb_nn(v_n, 568))->q_creates))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 569))->q_line, "функция может закончиться ошибкой: напиши  try f(),  f() or запасное  или  f() else err", ((tb_o_Node *)tb_nn(v_n, 569))->q_name);
    }
    { tb_o_Node * tb_v366 = ((tb_o_Func *)tb_nn(v_f, 570))->q_node; tb_o_Node * *tb_p366 = &(((tb_o_Node *)tb_nn(v_n, 570))->q_ref); *tb_p366 = tb_v366; }
    { _Bool tb_v367 = 1; _Bool *tb_p367 = &(((tb_o_Node *)tb_nn(v_n, 571))->q_has_ref); *tb_p367 = tb_v367; }
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 572))->q_node, 572))->q_kids, 0LL, sizeof(tb_o_Node *), 572));
    f_check__order_args(v_c, v_n, v_params, 0);
    {
        long long tb_end368 = ((((tb_o_Node *)tb_nn(v_n, 574))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end368; v_i++) {
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
                    { long long tb_v369 = v_t; long long *tb_p369 = &(((tb_o_Node *)tb_nn(v_p, 585))->q_ty); *tb_p369 = tb_v369; }
                } else if ((((tb_o_Node *)tb_nn(v_p, 586))->q_ty != v_t)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 587))->q_line, "этот ярлык уже был другого типа", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_p, 587))->q_ty));
                }
            } else if ((!f_check__fits(((tb_o_Node *)tb_nn(v_p, 588))->q_ty, v_t))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_value, 589))->q_line, "параметр ждёт другой тип", ((tb_o_Node *)tb_nn(v_p, 589))->q_name);
            }
            { long long tb_v370 = ((tb_o_Node *)tb_nn(v_p, 590))->q_ty; long long *tb_p370 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 590))->q_kids, v_i, sizeof(tb_o_Node *), 590)), 590))->q_ty); *tb_p370 = tb_v370; }
        }
    }
    { long long tb_v371 = ((tb_o_Comp *)tb_nn(v_c, 591))->q_current; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 591))->q_call_from) = tb_v371; }
    { long long tb_v372 = v_fi; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 592))->q_call_to) = tb_v372; }
    { _Bool tb_v373 = (((tb_o_Comp *)tb_nn(v_c, 593))->q_loop_depth > 0LL); tb_badd(((tb_o_Comp *)tb_nn(v_c, 593))->q_call_loop, tb_v373); }
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
        { tb_list * tb_v374 = v_fresh_loops; tb_list * *tb_p374 = &(((tb_o_Comp *)tb_nn(v_c, 609))->q_loops); *tb_p374 = tb_v374; }
        { long long tb_v375 = (-1LL); long long *tb_p375 = &(((tb_o_Comp *)tb_nn(v_c, 610))->q_par_base); *tb_p375 = tb_v375; }
        { long long tb_v376 = (-1LL); long long *tb_p376 = &(((tb_o_Comp *)tb_nn(v_c, 611))->q_temp_base); *tb_p376 = tb_v376; }
        { long long tb_v377 = 0LL; long long *tb_p377 = &(((tb_o_Comp *)tb_nn(v_c, 612))->q_piece_depth); *tb_p377 = tb_v377; }
        { long long tb_v378 = (((tb_o_Comp *)tb_nn(v_c, 613))->q_vars)->len; long long *tb_p378 = &(((tb_o_Comp *)tb_nn(v_c, 613))->q_var_base); *tb_p378 = tb_v378; }
        f_check__check_func(v_c, v_fi);
        { long long tb_v379 = v_saved_pieces; long long *tb_p379 = &(((tb_o_Comp *)tb_nn(v_c, 615))->q_piece_depth); *tb_p379 = tb_v379; }
        { tb_list * tb_v380 = v_saved_loop_stack; tb_list * *tb_p380 = &(((tb_o_Comp *)tb_nn(v_c, 616))->q_loops); *tb_p380 = tb_v380; }
        { long long tb_v381 = v_saved_par; long long *tb_p381 = &(((tb_o_Comp *)tb_nn(v_c, 617))->q_par_base); *tb_p381 = tb_v381; }
        { long long tb_v382 = v_saved_temp; long long *tb_p382 = &(((tb_o_Comp *)tb_nn(v_c, 618))->q_temp_base); *tb_p382 = tb_v382; }
        { long long tb_v383 = v_saved_temp_loops; long long *tb_p383 = &(((tb_o_Comp *)tb_nn(v_c, 619))->q_temp_loops); *tb_p383 = tb_v383; }
        { long long tb_v384 = v_saved_base; long long *tb_p384 = &(((tb_o_Comp *)tb_nn(v_c, 620))->q_var_base); *tb_p384 = tb_v384; }
        { const char * tb_v385 = v_saved_mod; const char * *tb_p385 = &(((tb_o_Comp *)tb_nn(v_c, 621))->q_cur_mod); *tb_p385 = tb_v385; }
        { const char * tb_v386 = v_saved_file; const char * *tb_p386 = &(((tb_o_Comp *)tb_nn(v_c, 622))->q_file_name); *tb_p386 = tb_v386; }
        { long long tb_v387 = v_saved_current; long long *tb_p387 = &(((tb_o_Comp *)tb_nn(v_c, 623))->q_current); *tb_p387 = tb_v387; }
        { long long tb_v388 = v_saved_loops; long long *tb_p388 = &(((tb_o_Comp *)tb_nn(v_c, 624))->q_loop_depth); *tb_p388 = tb_v388; }
        f_check__cut_vars(v_c, v_saved_vars);
        { long long tb_v389 = v_saved_depth; long long *tb_p389 = &(((tb_o_Comp *)tb_nn(v_c, 626))->q_depth); *tb_p389 = tb_v389; }
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
    { const char * tb_v390 = "call"; const char * *tb_p390 = &(((tb_o_Node *)tb_nn(v_n, 642))->q_kind); *tb_p390 = tb_v390; }
    tb_list * v_rest = tb_list_new(sizeof(tb_o_Node *));
    {
        long long tb_end391 = ((((tb_o_Node *)tb_nn(v_n, 644))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end391; v_i++) {
            { tb_o_Node * tb_v392 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 645))->q_kids, v_i, sizeof(tb_o_Node *), 645)); *(tb_o_Node * *)tb_grow(v_rest) = tb_v392; }
        }
    }
    { tb_list * tb_v393 = v_rest; tb_list * *tb_p393 = &(((tb_o_Node *)tb_nn(v_n, 646))->q_kids); *tb_p393 = tb_v393; }
    return f_check__check_user_call(v_c, v_n, v_f);
    return v_r;
}

static long long f_check__check_dict_method(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_t) {
    long long v_r = 0LL;
    long long v_args = ((((tb_o_Node *)tb_nn(v_n, 650))->q_kids)->len - 1LL);
    tb_o_Type * v_d = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 651))->q_types, v_t, sizeof(tb_o_Type *), 651));
    if ((({ const char *tb_s394 = ((tb_o_Node *)tb_nn(v_n, 652))->q_name; ((unsigned char)tb_s394[0] == 104 && (unsigned char)tb_s394[1] == 97 && (unsigned char)tb_s394[2] == 115 && tb_s394[3] == 0); }) || ({ const char *tb_s395 = ((tb_o_Node *)tb_nn(v_n, 652))->q_name; ((unsigned char)tb_s395[0] == 114 && (unsigned char)tb_s395[1] == 101 && (unsigned char)tb_s395[2] == 109 && (unsigned char)tb_s395[3] == 111 && (unsigned char)tb_s395[4] == 118 && (unsigned char)tb_s395[5] == 101 && tb_s395[6] == 0); }))) {
        if ((v_args != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 654))->q_line, "нужен ровно один ключ", ((tb_o_Node *)tb_nn(v_n, 654))->q_name);
        }
        long long v_k = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 655))->q_kids, 1LL, sizeof(tb_o_Node *), 655)), 655))->q_kids, 0LL, sizeof(tb_o_Node *), 655)));
        if ((v_k != ((tb_o_Type *)tb_nn(v_d, 656))->q_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 657))->q_line, "ключ у этого dict другого типа", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_d, 657))->q_key));
        }
        { long long tb_v396 = ((tb_o_Type *)tb_nn(v_d, 658))->q_key; long long *tb_p396 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 658))->q_kids, 1LL, sizeof(tb_o_Node *), 658)), 658))->q_ty); *tb_p396 = tb_v396; }
        if (({ const char *tb_s397 = ((tb_o_Node *)tb_nn(v_n, 659))->q_name; ((unsigned char)tb_s397[0] == 104 && (unsigned char)tb_s397[1] == 97 && (unsigned char)tb_s397[2] == 115 && tb_s397[3] == 0); })) {
            return 3LL;
        }
        return 0LL;
    }
    if ((({ const char *tb_s398 = ((tb_o_Node *)tb_nn(v_n, 662))->q_name; ((unsigned char)tb_s398[0] == 107 && (unsigned char)tb_s398[1] == 101 && (unsigned char)tb_s398[2] == 121 && (unsigned char)tb_s398[3] == 115 && tb_s398[4] == 0); }) || ({ const char *tb_s399 = ((tb_o_Node *)tb_nn(v_n, 662))->q_name; ((unsigned char)tb_s399[0] == 118 && (unsigned char)tb_s399[1] == 97 && (unsigned char)tb_s399[2] == 108 && (unsigned char)tb_s399[3] == 117 && (unsigned char)tb_s399[4] == 101 && (unsigned char)tb_s399[5] == 115 && tb_s399[6] == 0); }))) {
        if ((v_args != 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 664))->q_line, "пишется без значений", ((tb_o_Node *)tb_nn(v_n, 664))->q_name);
        }
        if (({ const char *tb_s400 = ((tb_o_Node *)tb_nn(v_n, 665))->q_name; ((unsigned char)tb_s400[0] == 107 && (unsigned char)tb_s400[1] == 101 && (unsigned char)tb_s400[2] == 121 && (unsigned char)tb_s400[3] == 115 && tb_s400[4] == 0); })) {
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
    if (((({ const char *tb_s401 = ((tb_o_Node *)tb_nn(v_base, 673))->q_kind; ((unsigned char)tb_s401[0] == 110 && (unsigned char)tb_s401[1] == 97 && (unsigned char)tb_s401[2] == 109 && (unsigned char)tb_s401[3] == 101 && tb_s401[4] == 0); }) && (f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_base, 673))->q_name) < 0LL)) && f_check__is_module(v_c, ((tb_o_Node *)tb_nn(v_base, 673))->q_name))) {
        return f_check__check_module_call(v_c, v_n);
    }
    long long v_t = f_check__check_value(v_c, v_base);
    long long v_args = ((((tb_o_Node *)tb_nn(v_n, 676))->q_kids)->len - 1LL);
    {
        long long tb_end402 = ((((tb_o_Node *)tb_nn(v_n, 677))->q_kids)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end402; v_i++) {
            if (({ const char *tb_s403 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 678))->q_kids, v_i, sizeof(tb_o_Node *), 678)), 678))->q_name; !(tb_s403[0] == 0); })) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 679))->q_line, "у методов аргументы пишутся без имён", "");
            }
        }
    }
    if ((((({ const char *tb_s404 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s404[0] == 97 && (unsigned char)tb_s404[1] == 100 && (unsigned char)tb_s404[2] == 100 && tb_s404[3] == 0); }) || ({ const char *tb_s405 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s405[0] == 112 && (unsigned char)tb_s405[1] == 111 && (unsigned char)tb_s405[2] == 112 && tb_s405[3] == 0); })) || ({ const char *tb_s406 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s406[0] == 114 && (unsigned char)tb_s406[1] == 101 && (unsigned char)tb_s406[2] == 109 && (unsigned char)tb_s406[3] == 111 && (unsigned char)tb_s406[4] == 118 && (unsigned char)tb_s406[5] == 101 && tb_s406[6] == 0); })) || ({ const char *tb_s407 = ((tb_o_Node *)tb_nn(v_n, 680))->q_name; ((unsigned char)tb_s407[0] == 99 && (unsigned char)tb_s407[1] == 108 && (unsigned char)tb_s407[2] == 101 && (unsigned char)tb_s407[3] == 97 && (unsigned char)tb_s407[4] == 114 && tb_s407[5] == 0); }))) {
        f_check__no_outside_change(v_c, v_base);
    }
    if (({ const char *tb_s408 = ((tb_o_Node *)tb_nn(v_n, 682))->q_name; ((unsigned char)tb_s408[0] == 99 && (unsigned char)tb_s408[1] == 108 && (unsigned char)tb_s408[2] == 101 && (unsigned char)tb_s408[3] == 97 && (unsigned char)tb_s408[4] == 114 && tb_s408[5] == 0); })) {
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
    if (({ const char *tb_s409 = ((tb_o_Node *)tb_nn(v_n, 696))->q_name; ((unsigned char)tb_s409[0] == 97 && (unsigned char)tb_s409[1] == 100 && (unsigned char)tb_s409[2] == 100 && tb_s409[3] == 0); })) {
        if ((v_args != 1LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 698))->q_line, ".add кладёт ровно одно значение", "");
        }
        long long v_v = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 699))->q_kids, 1LL, sizeof(tb_o_Node *), 699)), 699))->q_kids, 0LL, sizeof(tb_o_Node *), 699)));
        if ((!f_check__fits(v_elem, v_v))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 701))->q_line, "в этот список кладут другой тип", f_check__type_name(v_c, v_elem));
        }
        { long long tb_v410 = v_elem; long long *tb_p410 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 702))->q_kids, 1LL, sizeof(tb_o_Node *), 702)), 702))->q_ty); *tb_p410 = tb_v410; }
        return 0LL;
    }
    if (({ const char *tb_s411 = ((tb_o_Node *)tb_nn(v_n, 704))->q_name; ((unsigned char)tb_s411[0] == 112 && (unsigned char)tb_s411[1] == 111 && (unsigned char)tb_s411[2] == 112 && tb_s411[3] == 0); })) {
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
        { long long tb_v412 = 2LL; long long *tb_p412 = &(v_i); *tb_p412 += tb_v412; }
    }
    return f_check__dict_of(v_c, v_key, v_value);
    return v_r;
}

static _Bool f_check__can_fail_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if (({ const char *tb_s413 = ((tb_o_Node *)tb_nn(v_n, 735))->q_kind; ((unsigned char)tb_s413[0] == 99 && (unsigned char)tb_s413[1] == 97 && (unsigned char)tb_s413[2] == 108 && (unsigned char)tb_s413[3] == 108 && tb_s413[4] == 0); })) {
        if (f_check__is_builtin(((tb_o_Node *)tb_nn(v_n, 736))->q_name)) {
            return f_check__builtin_can_fail(((tb_o_Node *)tb_nn(v_n, 737))->q_name);
        }
        if (((f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 738))->q_name) >= 0LL) || (f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_n, 738))->q_name) >= 0LL))) {
            return 0;
        }
        long long v_f = f_check__find_func(v_c, ((tb_o_Node *)tb_nn(v_n, 740))->q_name);
        return ((v_f >= 0LL) && ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 741))->q_funcs, v_f, sizeof(tb_o_Func *), 741)), 741))->q_node, 741))->q_can_fail);
    }
    if ((((({ const char *tb_s414 = ((tb_o_Node *)tb_nn(v_n, 742))->q_kind; ((unsigned char)tb_s414[0] == 109 && (unsigned char)tb_s414[1] == 101 && (unsigned char)tb_s414[2] == 116 && (unsigned char)tb_s414[3] == 104 && (unsigned char)tb_s414[4] == 111 && (unsigned char)tb_s414[5] == 100 && tb_s414[6] == 0); }) && ({ const char *tb_s415 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 742))->q_kids, 0LL, sizeof(tb_o_Node *), 742)), 742))->q_kind; ((unsigned char)tb_s415[0] == 110 && (unsigned char)tb_s415[1] == 97 && (unsigned char)tb_s415[2] == 109 && (unsigned char)tb_s415[3] == 101 && tb_s415[4] == 0); })) && (f_check__find_var(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 742))->q_kids, 0LL, sizeof(tb_o_Node *), 742)), 742))->q_name) < 0LL)) && f_check__is_module(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 742))->q_kids, 0LL, sizeof(tb_o_Node *), 742)), 742))->q_name))) {
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
    { _Bool tb_v416 = 1; _Bool *tb_p416 = &(((tb_o_Node *)tb_nn(v_n, 751))->q_creates); *tb_p416 = tb_v416; }
    return f_check__check_expr(v_c, v_n);
    return v_r;
}

static long long f_check__check_expr(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_r = 0LL;
    long long v_t = (-1LL);
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 756))->q_kind;
    if (({ const char *tb_s417 = v_k; ((unsigned char)tb_s417[0] == 110 && (unsigned char)tb_s417[1] == 117 && (unsigned char)tb_s417[2] == 109 && (unsigned char)tb_s417[3] == 98 && (unsigned char)tb_s417[4] == 101 && (unsigned char)tb_s417[5] == 114 && tb_s417[6] == 0); })) {
        v_t = 1LL;
    } else if (({ const char *tb_s418 = v_k; ((unsigned char)tb_s418[0] == 100 && (unsigned char)tb_s418[1] == 101 && (unsigned char)tb_s418[2] == 99 && (unsigned char)tb_s418[3] == 105 && (unsigned char)tb_s418[4] == 109 && (unsigned char)tb_s418[5] == 97 && (unsigned char)tb_s418[6] == 108 && tb_s418[7] == 0); })) {
        v_t = 2LL;
    } else if (({ const char *tb_s419 = v_k; ((unsigned char)tb_s419[0] == 115 && (unsigned char)tb_s419[1] == 116 && (unsigned char)tb_s419[2] == 114 && (unsigned char)tb_s419[3] == 105 && (unsigned char)tb_s419[4] == 110 && (unsigned char)tb_s419[5] == 103 && tb_s419[6] == 0); })) {
        v_t = 4LL;
    } else if (({ const char *tb_s420 = v_k; ((unsigned char)tb_s420[0] == 98 && (unsigned char)tb_s420[1] == 111 && (unsigned char)tb_s420[2] == 111 && (unsigned char)tb_s420[3] == 108 && tb_s420[4] == 0); })) {
        v_t = 3LL;
    } else if (({ const char *tb_s421 = v_k; ((unsigned char)tb_s421[0] == 110 && (unsigned char)tb_s421[1] == 97 && (unsigned char)tb_s421[2] == 109 && (unsigned char)tb_s421[3] == 101 && tb_s421[4] == 0); })) {
        long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_n, 766))->q_name);
        if ((v_v < 0LL)) {
            if ((f_check__find_func(v_c, ((tb_o_Node *)tb_nn(v_n, 768))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 769))->q_line, "у функции нужны скобки", ((tb_o_Node *)tb_nn(v_n, 769))->q_name);
            }
            if ((f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_n, 770))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 771))->q_line, "чтобы создать obj, нужны скобки", ({ const char *tb_c422_0 = ((tb_o_Node *)tb_nn(v_n, 771))->q_name; long long tb_c422_0n = (long long)strlen(tb_c422_0); const char *tb_c422_1 = "(...)"; long long tb_c422_1n = sizeof("(...)") - 1; char *tb_c422r = tb_mem(tb_c422_0n + tb_c422_1n + 1), *tb_c422rw = tb_c422r; memcpy(tb_c422rw, tb_c422_0, tb_c422_0n); tb_c422rw += tb_c422_0n; memcpy(tb_c422rw, tb_c422_1, tb_c422_1n); tb_c422rw += tb_c422_1n; *tb_c422rw = 0; (const char *)tb_c422r; }));
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
        { _Bool tb_v423 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 780))->q_vars, v_v, sizeof(tb_o_Var *), 780)), 780))->q_is_link; _Bool *tb_p423 = &(((tb_o_Node *)tb_nn(v_n, 780))->q_is_link); *tb_p423 = tb_v423; }
        v_t = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 781))->q_vars, v_v, sizeof(tb_o_Var *), 781)), 781))->q_type;
    } else if (({ const char *tb_s424 = v_k; ((unsigned char)tb_s424[0] == 108 && (unsigned char)tb_s424[1] == 105 && (unsigned char)tb_s424[2] == 115 && (unsigned char)tb_s424[3] == 116 && tb_s424[4] == 0); })) {
        v_t = f_check__check_list(v_c, v_n);
    } else if (({ const char *tb_s425 = v_k; ((unsigned char)tb_s425[0] == 100 && (unsigned char)tb_s425[1] == 105 && (unsigned char)tb_s425[2] == 99 && (unsigned char)tb_s425[3] == 116 && tb_s425[4] == 0); })) {
        v_t = f_check__check_dict(v_c, v_n);
    } else if (({ const char *tb_s426 = v_k; ((unsigned char)tb_s426[0] == 116 && (unsigned char)tb_s426[1] == 114 && (unsigned char)tb_s426[2] == 121 && tb_s426[3] == 0); })) {
        if ((((tb_o_Comp *)tb_nn(v_c, 787))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 788))->q_line, ({ const char *tb_c427_0 = "внутри "; long long tb_c427_0n = sizeof("внутри ") - 1; const char *tb_c427_1 = f_check__loop_word(v_c); long long tb_c427_1n = (long long)strlen(tb_c427_1); const char *tb_c427_2 = " try нельзя: ядро не может остановить всю функцию, пиши  or  или  else err"; long long tb_c427_2n = sizeof(" try нельзя: ядро не может остановить всю функцию, пиши  or  или  else err") - 1; char *tb_c427r = tb_mem(tb_c427_0n + tb_c427_1n + tb_c427_2n + 1), *tb_c427rw = tb_c427r; memcpy(tb_c427rw, tb_c427_0, tb_c427_0n); tb_c427rw += tb_c427_0n; memcpy(tb_c427rw, tb_c427_1, tb_c427_1n); tb_c427rw += tb_c427_1n; memcpy(tb_c427rw, tb_c427_2, tb_c427_2n); tb_c427rw += tb_c427_2n; *tb_c427rw = 0; (const char *)tb_c427r; }), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 789))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 790))->q_line, "внутри temp try нельзя: пиши  or  или  else err", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 791))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 792))->q_line, "внутри each ... by N try нельзя: пиши  or  или  else err", "");
        }
        v_t = f_check__check_handled(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 793))->q_kids, 0LL, sizeof(tb_o_Node *), 793)), "try");
    } else if (({ const char *tb_s428 = v_k; ((unsigned char)tb_s428[0] == 101 && (unsigned char)tb_s428[1] == 108 && (unsigned char)tb_s428[2] == 115 && (unsigned char)tb_s428[3] == 101 && (unsigned char)tb_s428[4] == 95 && (unsigned char)tb_s428[5] == 101 && (unsigned char)tb_s428[6] == 114 && (unsigned char)tb_s428[7] == 114 && tb_s428[8] == 0); })) {
        v_t = f_check__check_handled(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 795))->q_kids, 0LL, sizeof(tb_o_Node *), 795)), "else");
        f_check__open_scope(v_c);
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 797))->q_name, 4LL, 1, 0, ((tb_o_Node *)tb_nn(v_n, 797))->q_line);
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 798))->q_kids, 1LL, sizeof(tb_o_Node *), 798)));
        f_check__close_scope(v_c);
    } else if (({ const char *tb_s429 = v_k; ((unsigned char)tb_s429[0] == 99 && (unsigned char)tb_s429[1] == 97 && (unsigned char)tb_s429[2] == 108 && (unsigned char)tb_s429[3] == 108 && tb_s429[4] == 0); })) {
        v_t = f_check__check_call(v_c, v_n);
    } else if (({ const char *tb_s430 = v_k; ((unsigned char)tb_s430[0] == 109 && (unsigned char)tb_s430[1] == 101 && (unsigned char)tb_s430[2] == 116 && (unsigned char)tb_s430[3] == 104 && (unsigned char)tb_s430[4] == 111 && (unsigned char)tb_s430[5] == 100 && tb_s430[6] == 0); })) {
        v_t = f_check__check_method(v_c, v_n);
    } else if (({ const char *tb_s431 = v_k; ((unsigned char)tb_s431[0] == 98 && (unsigned char)tb_s431[1] == 105 && (unsigned char)tb_s431[2] == 110 && (unsigned char)tb_s431[3] == 111 && (unsigned char)tb_s431[4] == 112 && tb_s431[5] == 0); })) {
        v_t = f_check__check_binop(v_c, v_n);
    } else if (({ const char *tb_s432 = v_k; ((unsigned char)tb_s432[0] == 117 && (unsigned char)tb_s432[1] == 110 && (unsigned char)tb_s432[2] == 97 && (unsigned char)tb_s432[3] == 114 && (unsigned char)tb_s432[4] == 121 && tb_s432[5] == 0); })) {
        long long v_inner = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 807))->q_kids, 0LL, sizeof(tb_o_Node *), 807)));
        if (({ const char *tb_s433 = ((tb_o_Node *)tb_nn(v_n, 808))->q_value; ((unsigned char)tb_s433[0] == 110 && (unsigned char)tb_s433[1] == 111 && (unsigned char)tb_s433[2] == 116 && tb_s433[3] == 0); })) {
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
    } else if ((({ const char *tb_s434 = v_k; ((unsigned char)tb_s434[0] == 105 && (unsigned char)tb_s434[1] == 110 && (unsigned char)tb_s434[2] == 100 && (unsigned char)tb_s434[3] == 101 && (unsigned char)tb_s434[4] == 120 && tb_s434[5] == 0); }) && f_check__is_dict(v_c, f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 816))->q_kids, 0LL, sizeof(tb_o_Node *), 816)))))) {
        tb_o_Type * v_base = (*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 817))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 817))->q_kids, 0LL, sizeof(tb_o_Node *), 817)), 817))->q_ty, sizeof(tb_o_Type *), 817));
        if ((f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 818))->q_kids, 1LL, sizeof(tb_o_Node *), 818))) != ((tb_o_Type *)tb_nn(v_base, 818))->q_key)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 819))->q_line, "ключ у этого dict другого типа", f_check__type_name(v_c, ((tb_o_Type *)tb_nn(v_base, 819))->q_key));
        }
        v_t = ((tb_o_Type *)tb_nn(v_base, 820))->q_elem;
    } else if (({ const char *tb_s435 = v_k; ((unsigned char)tb_s435[0] == 105 && (unsigned char)tb_s435[1] == 110 && (unsigned char)tb_s435[2] == 100 && (unsigned char)tb_s435[3] == 101 && (unsigned char)tb_s435[4] == 120 && tb_s435[5] == 0); })) {
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
    } else if (((({ const char *tb_s436 = v_k; ((unsigned char)tb_s436[0] == 102 && (unsigned char)tb_s436[1] == 105 && (unsigned char)tb_s436[2] == 101 && (unsigned char)tb_s436[3] == 108 && (unsigned char)tb_s436[4] == 100 && tb_s436[5] == 0); }) && ({ const char *tb_s437 = ((tb_o_Node *)tb_nn(v_n, 832))->q_name; ((unsigned char)tb_s437[0] == 119 && (unsigned char)tb_s437[1] == 97 && (unsigned char)tb_s437[2] == 105 && (unsigned char)tb_s437[3] == 116 && tb_s437[4] == 0); })) && ({ const char *tb_s438 = f_check__kind_of(v_c, f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 832))->q_kids, 0LL, sizeof(tb_o_Node *), 832)))); ((unsigned char)tb_s438[0] == 116 && (unsigned char)tb_s438[1] == 97 && (unsigned char)tb_s438[2] == 115 && (unsigned char)tb_s438[3] == 107 && tb_s438[4] == 0); }))) {
        if ((((tb_o_Comp *)tb_nn(v_c, 833))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 834))->q_line, ({ const char *tb_c439_0 = "внутри "; long long tb_c439_0n = sizeof("внутри ") - 1; const char *tb_c439_1 = f_check__loop_word(v_c); long long tb_c439_1n = (long long)strlen(tb_c439_1); const char *tb_c439_2 = " .wait нельзя: забери готовое до цикла или после"; long long tb_c439_2n = sizeof(" .wait нельзя: забери готовое до цикла или после") - 1; char *tb_c439r = tb_mem(tb_c439_0n + tb_c439_1n + tb_c439_2n + 1), *tb_c439rw = tb_c439r; memcpy(tb_c439rw, tb_c439_0, tb_c439_0n); tb_c439rw += tb_c439_0n; memcpy(tb_c439rw, tb_c439_1, tb_c439_1n); tb_c439rw += tb_c439_1n; memcpy(tb_c439rw, tb_c439_2, tb_c439_2n); tb_c439rw += tb_c439_2n; *tb_c439rw = 0; (const char *)tb_c439r; }), "");
        }
        v_t = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 835))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 835))->q_kids, 0LL, sizeof(tb_o_Node *), 835)), 835))->q_ty, sizeof(tb_o_Type *), 835)), 835))->q_elem;
    } else if ((strcmp(v_k, "background") == 0)) {
        v_t = f_check__check_background(v_c, v_n);
    } else if (({ const char *tb_s440 = v_k; ((unsigned char)tb_s440[0] == 102 && (unsigned char)tb_s440[1] == 105 && (unsigned char)tb_s440[2] == 101 && (unsigned char)tb_s440[3] == 108 && (unsigned char)tb_s440[4] == 100 && tb_s440[5] == 0); })) {
        long long v_base = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 839))->q_kids, 0LL, sizeof(tb_o_Node *), 839)));
        if (({ const char *tb_s441 = f_check__kind_of(v_c, v_base); !((unsigned char)tb_s441[0] == 111 && (unsigned char)tb_s441[1] == 98 && (unsigned char)tb_s441[2] == 106 && tb_s441[3] == 0); })) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 841))->q_line, "точка с качеством бывает только у obj", ((tb_o_Node *)tb_nn(v_n, 841))->q_name);
        }
        tb_o_Node * v_o = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 842))->q_types, v_base, sizeof(tb_o_Type *), 842)), 842))->q_obj_node;
        long long v_field = f_check__find_field(v_o, ((tb_o_Node *)tb_nn(v_n, 843))->q_name);
        if ((v_field < 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 845))->q_line, "у этого obj нет такого качества", ((tb_o_Node *)tb_nn(v_n, 845))->q_name);
        }
        v_t = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 846))->q_kids, v_field, sizeof(tb_o_Node *), 846)), 846))->q_ty;
        { tb_o_Node * tb_v442 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 847))->q_kids, v_field, sizeof(tb_o_Node *), 847)); *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 847))->q_acc_field) = tb_v442; }
        { long long tb_v443 = ((tb_o_Comp *)tb_nn(v_c, 848))->q_current; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 848))->q_acc_func) = tb_v443; }
        { _Bool tb_v444 = (((tb_o_Comp *)tb_nn(v_c, 849))->q_loop_depth > 0LL); tb_badd(((tb_o_Comp *)tb_nn(v_c, 849))->q_acc_loop, tb_v444); }
    }
    { long long tb_v445 = v_t; long long *tb_p445 = &(((tb_o_Node *)tb_nn(v_n, 850))->q_ty); *tb_p445 = tb_v445; }
    if (({ const char *tb_s446 = v_k; ((unsigned char)tb_s446[0] == 105 && (unsigned char)tb_s446[1] == 110 && (unsigned char)tb_s446[2] == 100 && (unsigned char)tb_s446[3] == 101 && (unsigned char)tb_s446[4] == 120 && tb_s446[5] == 0); })) {
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
    if ((({ const char *tb_s447 = ((tb_o_Node *)tb_nn(v_a, 865))->q_kind; !((unsigned char)tb_s447[0] == 110 && (unsigned char)tb_s447[1] == 97 && (unsigned char)tb_s447[2] == 109 && (unsigned char)tb_s447[3] == 101 && tb_s447[4] == 0); }) || (!f_check__declared_before(v_c, ((tb_o_Node *)tb_nn(v_a, 865))->q_name, ((tb_o_Node *)tb_nn(v_loop, 865))->q_name)))) {
        return;
    }
    if ((!((f_check__is_list(v_c, ((tb_o_Node *)tb_nn(v_a, 867))->q_ty) && (!f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_a, 867))->q_ty))) || f_check__is_dict(v_c, ((tb_o_Node *)tb_nn(v_a, 867))->q_ty)))) {
        return;
    }
    tb_o_Node * v_i = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 869))->q_kids, 1LL, sizeof(tb_o_Node *), 869));
    tb_o_Node * v_plan = tb_new_Node("missing", 0LL, "", "", 0, 0, 0LL, 0, 0, 0, 0, 0, "", "", 0, 0, 0LL, tb_list_new(sizeof(tb_o_Node *)), 0, 0, 0, 0, 0, 0, tb_list_new(sizeof(tb_o_Node *)), tb_list_new(sizeof(tb_o_Node *)), 0, 0, tb_list_new(sizeof(tb_o_Var *)));
    if (({ const char *tb_s448 = ((tb_o_Node *)tb_nn(v_loop, 871))->q_kind; ((unsigned char)tb_s448[0] == 101 && (unsigned char)tb_s448[1] == 97 && (unsigned char)tb_s448[2] == 99 && (unsigned char)tb_s448[3] == 104 && tb_s448[4] == 0); })) {
        if (((!f_check__is_name_of(v_i, ((tb_o_Node *)tb_nn(v_loop, 872))->q_name)) || ((!f_check__is_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 872))->q_kids, 0LL, sizeof(tb_o_Node *), 872)), 872))->q_ty)) && (!f_check__is_dict(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 872))->q_kids, 0LL, sizeof(tb_o_Node *), 872)), 872))->q_ty))))) {
            return;
        }
    } else {
        if ((({ const char *tb_s449 = ((tb_o_Node *)tb_nn(v_i, 875))->q_kind; !((unsigned char)tb_s449[0] == 105 && (unsigned char)tb_s449[1] == 110 && (unsigned char)tb_s449[2] == 100 && (unsigned char)tb_s449[3] == 101 && (unsigned char)tb_s449[4] == 120 && tb_s449[5] == 0); }) || (!f_check__is_name_of((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_i, 875))->q_kids, 1LL, sizeof(tb_o_Node *), 875)), ((tb_o_Node *)tb_nn(v_loop, 875))->q_name)))) {
            return;
        }
        v_plan = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_i, 877))->q_kids, 0LL, sizeof(tb_o_Node *), 877));
        if ((((({ const char *tb_s450 = ((tb_o_Node *)tb_nn(v_plan, 878))->q_kind; !((unsigned char)tb_s450[0] == 110 && (unsigned char)tb_s450[1] == 97 && (unsigned char)tb_s450[2] == 109 && (unsigned char)tb_s450[3] == 101 && tb_s450[4] == 0); }) || (!f_check__declared_before(v_c, ((tb_o_Node *)tb_nn(v_plan, 878))->q_name, ((tb_o_Node *)tb_nn(v_loop, 878))->q_name))) || (!f_check__is_list(v_c, ((tb_o_Node *)tb_nn(v_plan, 878))->q_ty))) || f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_plan, 878))->q_ty))) {
            return;
        }
    }
    {
        long long tb_end451 = ((((tb_o_Node *)tb_nn(v_loop, 880))->q_pre_bases)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end451; v_j++) {
            if (((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 881))->q_pre_bases, v_j, sizeof(tb_o_Node *), 881)), 881))->q_name, ((tb_o_Node *)tb_nn(v_a, 881))->q_name) == 0) && (strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_loop, 881))->q_pre_idx, v_j, sizeof(tb_o_Node *), 881)), 881))->q_name, ((tb_o_Node *)tb_nn(v_plan, 881))->q_name) == 0))) {
                return;
            }
        }
    }
    { tb_o_Node * tb_v452 = v_a; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_loop, 883))->q_pre_bases) = tb_v452; }
    { tb_o_Node * tb_v453 = v_plan; *(tb_o_Node * *)tb_grow(((tb_o_Node *)tb_nn(v_loop, 884))->q_pre_idx) = tb_v453; }
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
        { _Bool tb_v454 = 1; _Bool *tb_p454 = &(((tb_o_Node *)tb_nn(v_n, 921))->q_creates); *tb_p454 = tb_v454; }
        { long long tb_v455 = v_type; long long *tb_p455 = &(((tb_o_Node *)tb_nn(v_n, 922))->q_ty); *tb_p455 = tb_v455; }
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 923))->q_name, v_type, ((tb_o_Node *)tb_nn(v_n, 923))->q_is_const, 0, ((tb_o_Node *)tb_nn(v_n, 923))->q_line);
        { _Bool tb_v456 = ((tb_o_Node *)tb_nn(v_n, 924))->q_is_shared; _Bool *tb_p456 = &(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 924))->q_vars, ((((tb_o_Comp *)tb_nn(v_c, 924))->q_vars)->len - 1LL), sizeof(tb_o_Var *), 924)), 924))->q_is_shared); *tb_p456 = tb_v456; }
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
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 933))->q_line, ({ const char *tb_c457_0 = "внутри "; long long tb_c457_0n = sizeof("внутри ") - 1; const char *tb_c457_1 = f_check__loop_word(v_c); long long tb_c457_1n = (long long)strlen(tb_c457_1); const char *tb_c457_2 = " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="; long long tb_c457_2n = sizeof(" нельзя менять переменную снаружи: сделай её shared и меняй через += или -=") - 1; char *tb_c457r = tb_mem(tb_c457_0n + tb_c457_1n + tb_c457_2n + 1), *tb_c457rw = tb_c457r; memcpy(tb_c457rw, tb_c457_0, tb_c457_0n); tb_c457rw += tb_c457_0n; memcpy(tb_c457rw, tb_c457_1, tb_c457_1n); tb_c457rw += tb_c457_1n; memcpy(tb_c457rw, tb_c457_2, tb_c457_2n); tb_c457rw += tb_c457_2n; *tb_c457rw = 0; (const char *)tb_c457r; }), ((tb_o_Node *)tb_nn(v_n, 933))->q_name);
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
    { _Bool tb_v458 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 940))->q_vars, v_v, sizeof(tb_o_Var *), 940)), 940))->q_is_link; _Bool *tb_p458 = &(((tb_o_Node *)tb_nn(v_n, 940))->q_is_link); *tb_p458 = tb_v458; }
    { long long tb_v459 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 941))->q_vars, v_v, sizeof(tb_o_Var *), 941)), 941))->q_type; long long *tb_p459 = &(((tb_o_Node *)tb_nn(v_n, 941))->q_ty); *tb_p459 = tb_v459; }
}

static void f_check__check_set(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 944))->q_kids, 0LL, sizeof(tb_o_Node *), 944));
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 945))->q_value;
    { _Bool tb_v460 = (({ const char *tb_s461 = ((tb_o_Node *)tb_nn(v_target, 946))->q_kind; ((unsigned char)tb_s461[0] == 110 && (unsigned char)tb_s461[1] == 97 && (unsigned char)tb_s461[2] == 109 && (unsigned char)tb_s461[3] == 101 && tb_s461[4] == 0); }) && (({ const char *tb_s462 = v_op; ((unsigned char)tb_s462[0] == 43 && (unsigned char)tb_s462[1] == 61 && tb_s462[2] == 0); }) || ({ const char *tb_s463 = v_op; ((unsigned char)tb_s463[0] == 45 && (unsigned char)tb_s463[1] == 61 && tb_s463[2] == 0); }))); _Bool *tb_p460 = &(((tb_o_Comp *)tb_nn(v_c, 946))->q_shared_ok); *tb_p460 = tb_v460; }
    long long v_to_ty = f_check__check_value(v_c, v_target);
    { _Bool tb_v464 = 0; _Bool *tb_p464 = &(((tb_o_Comp *)tb_nn(v_c, 948))->q_shared_ok); *tb_p464 = tb_v464; }
    f_check__need_place(v_c, v_target, "менять можно только переменную, u.age или nums[i]");
    long long v_r = f_check__root_var(v_c, v_target);
    if (((f_check__outside_temp(v_c, v_r) && ({ const char *tb_s465 = ((tb_o_Node *)tb_nn(v_target, 951))->q_kind; ((unsigned char)tb_s465[0] == 110 && (unsigned char)tb_s465[1] == 97 && (unsigned char)tb_s465[2] == 109 && (unsigned char)tb_s465[3] == 101 && tb_s465[4] == 0); })) && (!f_check__is_plain(((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 951))->q_vars, v_r, sizeof(tb_o_Var *), 951)), 951))->q_type)))) {
        f_check__temp_keep_out(v_c, ((tb_o_Node *)tb_nn(v_n, 952))->q_line, ((tb_o_Node *)tb_nn(v_target, 952))->q_name);
    }
    if ((f_check__outside_par(v_c, v_r) && ((tb_o_Comp *)tb_nn(v_c, 953))->q_par_vec)) {
        if (({ const char *tb_s466 = ((tb_o_Node *)tb_nn(v_target, 954))->q_kind; !((unsigned char)tb_s466[0] == 110 && (unsigned char)tb_s466[1] == 97 && (unsigned char)tb_s466[2] == 109 && (unsigned char)tb_s466[3] == 101 && tb_s466[4] == 0); })) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 955))->q_line, "внутри by vec нельзя менять списки и obj снаружи цикла", "");
        }
        tb_o_Var * v_var = (*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 956))->q_vars, v_r, sizeof(tb_o_Var *), 956));
        if (((({ const char *tb_s467 = v_op; !((unsigned char)tb_s467[0] == 43 && (unsigned char)tb_s467[1] == 61 && tb_s467[2] == 0); }) && ({ const char *tb_s468 = v_op; !((unsigned char)tb_s468[0] == 45 && (unsigned char)tb_s468[1] == 61 && tb_s468[2] == 0); })) || (!f_check__is_number(((tb_o_Var *)tb_nn(v_var, 957))->q_type)))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 958))->q_line, "внутри by vec число снаружи можно только копить через += или -=", ((tb_o_Node *)tb_nn(v_target, 958))->q_name);
        }
        _Bool v_found = 0;
        {
            tb_list * tb_l469 = ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 960))->q_par_node, 960))->q_caps;
            for (long long tb_i469 = 0; tb_i469 < tb_l469->len; tb_i469++) {
                tb_o_Var * const v_old = ((tb_o_Var * *)tb_l469->data)[tb_i469];
                if (((strcmp(((tb_o_Var *)tb_nn(v_old, 961))->q_name, ((tb_o_Var *)tb_nn(v_var, 961))->q_name) == 0) && ((tb_o_Var *)tb_nn(v_old, 961))->q_is_shared)) {
                    v_found = 1;
                    if ((strcmp(((tb_o_Var *)tb_nn(v_old, 963))->q_op, v_op) != 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 964))->q_line, "в by vec одну переменную копят одним знаком: или +=, или -=", ((tb_o_Node *)tb_nn(v_target, 964))->q_name);
                    }
                }
            }
        }
        if ((!v_found)) {
            { tb_o_Var * tb_v470 = tb_new_Var(((tb_o_Var *)tb_nn(v_var, 966))->q_name, ((tb_o_Var *)tb_nn(v_var, 966))->q_type, 0, ((tb_o_Var *)tb_nn(v_var, 966))->q_is_link, 0LL, 1, v_op); *(tb_o_Var * *)tb_grow(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 966))->q_par_node, 966))->q_caps) = tb_v470; }
        }
    } else if (f_check__outside_par(v_c, v_r)) {
        if (({ const char *tb_s471 = ((tb_o_Node *)tb_nn(v_target, 968))->q_kind; !((unsigned char)tb_s471[0] == 110 && (unsigned char)tb_s471[1] == 97 && (unsigned char)tb_s471[2] == 109 && (unsigned char)tb_s471[3] == 101 && tb_s471[4] == 0); })) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 969))->q_line, ({ const char *tb_c472_0 = "внутри "; long long tb_c472_0n = sizeof("внутри ") - 1; const char *tb_c472_1 = f_check__loop_word(v_c); long long tb_c472_1n = (long long)strlen(tb_c472_1); const char *tb_c472_2 = " нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)"; long long tb_c472_2n = sizeof(" нельзя менять списки и obj снаружи цикла (свои, созданные внутри, — можно)") - 1; char *tb_c472r = tb_mem(tb_c472_0n + tb_c472_1n + tb_c472_2n + 1), *tb_c472rw = tb_c472r; memcpy(tb_c472rw, tb_c472_0, tb_c472_0n); tb_c472rw += tb_c472_0n; memcpy(tb_c472rw, tb_c472_1, tb_c472_1n); tb_c472rw += tb_c472_1n; memcpy(tb_c472rw, tb_c472_2, tb_c472_2n); tb_c472rw += tb_c472_2n; *tb_c472rw = 0; (const char *)tb_c472r; }), "");
        }
        if ((!((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 970))->q_vars, v_r, sizeof(tb_o_Var *), 970)), 970))->q_is_shared)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 971))->q_line, ({ const char *tb_c473_0 = "внутри "; long long tb_c473_0n = sizeof("внутри ") - 1; const char *tb_c473_1 = f_check__loop_word(v_c); long long tb_c473_1n = (long long)strlen(tb_c473_1); const char *tb_c473_2 = " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="; long long tb_c473_2n = sizeof(" нельзя менять переменную снаружи: сделай её shared и меняй через += или -=") - 1; char *tb_c473r = tb_mem(tb_c473_0n + tb_c473_1n + tb_c473_2n + 1), *tb_c473rw = tb_c473r; memcpy(tb_c473rw, tb_c473_0, tb_c473_0n); tb_c473rw += tb_c473_0n; memcpy(tb_c473rw, tb_c473_1, tb_c473_1n); tb_c473rw += tb_c473_1n; memcpy(tb_c473rw, tb_c473_2, tb_c473_2n); tb_c473rw += tb_c473_2n; *tb_c473rw = 0; (const char *)tb_c473r; }), ((tb_o_Node *)tb_nn(v_target, 971))->q_name);
        }
        if ((({ const char *tb_s474 = v_op; !((unsigned char)tb_s474[0] == 43 && (unsigned char)tb_s474[1] == 61 && tb_s474[2] == 0); }) && ({ const char *tb_s475 = v_op; !((unsigned char)tb_s475[0] == 45 && (unsigned char)tb_s475[1] == 61 && tb_s475[2] == 0); }))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 973))->q_line, "shared внутри on all cores меняется только через += или -=", ((tb_o_Node *)tb_nn(v_target, 973))->q_name);
        }
    }
    long long v_t = f_check__check_value(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 974))->q_kids, 1LL, sizeof(tb_o_Node *), 974)));
    { long long tb_v476 = v_to_ty; long long *tb_p476 = &(((tb_o_Node *)tb_nn(v_n, 975))->q_ty); *tb_p476 = tb_v476; }
    if (({ const char *tb_s477 = v_op; ((unsigned char)tb_s477[0] == 61 && tb_s477[1] == 0); })) {
        if ((!f_check__fits(v_to_ty, v_t))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 978))->q_line, "здесь лежит другой тип", f_check__type_name(v_c, v_to_ty));
        }
        return;
    }
    if (((v_to_ty == 4LL) && ({ const char *tb_s478 = v_op; ((unsigned char)tb_s478[0] == 43 && (unsigned char)tb_s478[1] == 61 && tb_s478[2] == 0); }))) {
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
    { long long tb_v479 = v_t; long long *tb_p479 = &(((tb_o_Node *)tb_nn(v_n, 996))->q_ty); *tb_p479 = tb_v479; }
    f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 997))->q_name, v_t, 0, 1, ((tb_o_Node *)tb_nn(v_n, 997))->q_line);
}

static void f_check__check_multi(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1000))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 1000))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 1000));
    tb_o_Node * v_call = v_value;
    if ((({ const char *tb_s480 = ((tb_o_Node *)tb_nn(v_value, 1002))->q_kind; ((unsigned char)tb_s480[0] == 116 && (unsigned char)tb_s480[1] == 114 && (unsigned char)tb_s480[2] == 121 && tb_s480[3] == 0); }) || ({ const char *tb_s481 = ((tb_o_Node *)tb_nn(v_value, 1002))->q_kind; ((unsigned char)tb_s481[0] == 101 && (unsigned char)tb_s481[1] == 108 && (unsigned char)tb_s481[2] == 115 && (unsigned char)tb_s481[3] == 101 && (unsigned char)tb_s481[4] == 95 && (unsigned char)tb_s481[5] == 101 && (unsigned char)tb_s481[6] == 114 && (unsigned char)tb_s481[7] == 114 && tb_s481[8] == 0); }))) {
        v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 1003))->q_kids, 0LL, sizeof(tb_o_Node *), 1003));
    }
    if ((({ const char *tb_s482 = ((tb_o_Node *)tb_nn(v_call, 1004))->q_kind; !((unsigned char)tb_s482[0] == 99 && (unsigned char)tb_s482[1] == 97 && (unsigned char)tb_s482[2] == 108 && (unsigned char)tb_s482[3] == 108 && tb_s482[4] == 0); }) && ({ const char *tb_s483 = ((tb_o_Node *)tb_nn(v_call, 1004))->q_kind; !((unsigned char)tb_s483[0] == 109 && (unsigned char)tb_s483[1] == 101 && (unsigned char)tb_s483[2] == 116 && (unsigned char)tb_s483[3] == 104 && (unsigned char)tb_s483[4] == 111 && (unsigned char)tb_s483[5] == 100 && tb_s483[6] == 0); }))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1005))->q_line, "справа должен быть вызов функции с gives", "");
    }
    long long v_t = f_check__check_expr(v_c, v_value);
    long long v_names = ((((tb_o_Node *)tb_nn(v_n, 1007))->q_kids)->len - 1LL);
    if (({ const char *tb_s484 = f_check__kind_of(v_c, v_t); !((unsigned char)tb_s484[0] == 109 && (unsigned char)tb_s484[1] == 117 && (unsigned char)tb_s484[2] == 108 && (unsigned char)tb_s484[3] == 116 && (unsigned char)tb_s484[4] == 105 && tb_s484[5] == 0); })) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1009))->q_line, "у этой функции один выход, пиши  x = ...", ((tb_o_Node *)tb_nn(v_call, 1009))->q_name);
    }
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1010))->q_types, v_t, sizeof(tb_o_Type *), 1010)), 1010))->q_func_node, 1010))->q_kids, 2LL, sizeof(tb_o_Node *), 1010));
    if (((((tb_o_Node *)tb_nn(v_outputs, 1011))->q_kids)->len != v_names)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1012))->q_line, "у функции другое число выходов", ((tb_o_Node *)tb_nn(v_call, 1012))->q_name);
    }
    {
        long long tb_end485 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end485; v_i++) {
            tb_o_Node * v_name = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1014))->q_kids, v_i, sizeof(tb_o_Node *), 1014));
            long long v_out = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1015))->q_kids, v_i, sizeof(tb_o_Node *), 1015)), 1015))->q_ty;
            long long v_v = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_name, 1016))->q_name);
            if ((v_v < 0LL)) {
                { _Bool tb_v486 = 1; _Bool *tb_p486 = &(((tb_o_Node *)tb_nn(v_name, 1018))->q_creates); *tb_p486 = tb_v486; }
                f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_name, 1019))->q_name, v_out, 0, 0, ((tb_o_Node *)tb_nn(v_name, 1019))->q_line);
            } else {
                if (f_check__outside_par(v_c, v_v)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_name, 1022))->q_line, ({ const char *tb_c487_0 = "внутри "; long long tb_c487_0n = sizeof("внутри ") - 1; const char *tb_c487_1 = f_check__loop_word(v_c); long long tb_c487_1n = (long long)strlen(tb_c487_1); const char *tb_c487_2 = " нельзя менять переменную снаружи: сделай её shared и меняй через += или -="; long long tb_c487_2n = sizeof(" нельзя менять переменную снаружи: сделай её shared и меняй через += или -=") - 1; char *tb_c487r = tb_mem(tb_c487_0n + tb_c487_1n + tb_c487_2n + 1), *tb_c487rw = tb_c487r; memcpy(tb_c487rw, tb_c487_0, tb_c487_0n); tb_c487rw += tb_c487_0n; memcpy(tb_c487rw, tb_c487_1, tb_c487_1n); tb_c487rw += tb_c487_1n; memcpy(tb_c487rw, tb_c487_2, tb_c487_2n); tb_c487rw += tb_c487_2n; *tb_c487rw = 0; (const char *)tb_c487r; }), ((tb_o_Node *)tb_nn(v_name, 1022))->q_name);
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
                { _Bool tb_v488 = ((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1029))->q_vars, v_v, sizeof(tb_o_Var *), 1029)), 1029))->q_is_link; _Bool *tb_p488 = &(((tb_o_Node *)tb_nn(v_name, 1029))->q_is_link); *tb_p488 = tb_v488; }
            }
            { long long tb_v489 = v_out; long long *tb_p489 = &(((tb_o_Node *)tb_nn(v_name, 1030))->q_ty); *tb_p489 = tb_v489; }
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
    if ((({ const char *tb_s490 = ((tb_o_Node *)tb_nn(v_f, 1046))->q_name; ((unsigned char)tb_s490[0] == 109 && (unsigned char)tb_s490[1] == 97 && (unsigned char)tb_s490[2] == 105 && (unsigned char)tb_s490[3] == 110 && tb_s490[4] == 0); }) && (v_t != 0LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1047))->q_line, "main ничего не возвращает: пиши просто return", "");
    }
    if ((((tb_o_Node *)tb_nn(v_f, 1048))->q_ty < 0LL)) {
        { long long tb_v491 = v_t; long long *tb_p491 = &(((tb_o_Node *)tb_nn(v_f, 1049))->q_ty); *tb_p491 = tb_v491; }
    } else if (((((tb_o_Node *)tb_nn(v_f, 1050))->q_ty == 1LL) && (v_t == 2LL))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1051))->q_line, "раньше функция вернула int, а тут dec: сделай оба числа одного типа", "");
    } else if ((!f_check__fits(((tb_o_Node *)tb_nn(v_f, 1052))->q_ty, v_t))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1053))->q_line, "раньше эта функция вернула другой тип", f_check__type_name(v_c, ((tb_o_Node *)tb_nn(v_f, 1053))->q_ty));
    }
}

static void f_check__check_statement(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1056))->q_kind;
    if (({ const char *tb_s492 = v_k; ((unsigned char)tb_s492[0] == 97 && (unsigned char)tb_s492[1] == 115 && (unsigned char)tb_s492[2] == 115 && (unsigned char)tb_s492[3] == 105 && (unsigned char)tb_s492[4] == 103 && (unsigned char)tb_s492[5] == 110 && tb_s492[6] == 0); })) {
        f_check__check_assign(v_c, v_n);
    } else if (({ const char *tb_s493 = v_k; ((unsigned char)tb_s493[0] == 115 && (unsigned char)tb_s493[1] == 101 && (unsigned char)tb_s493[2] == 116 && tb_s493[3] == 0); })) {
        f_check__check_set(v_c, v_n);
    } else if (({ const char *tb_s494 = v_k; ((unsigned char)tb_s494[0] == 108 && (unsigned char)tb_s494[1] == 105 && (unsigned char)tb_s494[2] == 110 && (unsigned char)tb_s494[3] == 107 && (unsigned char)tb_s494[4] == 100 && (unsigned char)tb_s494[5] == 101 && (unsigned char)tb_s494[6] == 99 && (unsigned char)tb_s494[7] == 108 && tb_s494[8] == 0); })) {
        f_check__check_linkdecl(v_c, v_n);
    } else if (({ const char *tb_s495 = v_k; ((unsigned char)tb_s495[0] == 109 && (unsigned char)tb_s495[1] == 117 && (unsigned char)tb_s495[2] == 108 && (unsigned char)tb_s495[3] == 116 && (unsigned char)tb_s495[4] == 105 && tb_s495[5] == 0); })) {
        f_check__check_multi(v_c, v_n);
    } else if (({ const char *tb_s496 = v_k; ((unsigned char)tb_s496[0] == 105 && (unsigned char)tb_s496[1] == 102 && tb_s496[2] == 0); })) {
        long long v_i = 0LL;
        while (((v_i + 1LL) < (((tb_o_Node *)tb_nn(v_n, 1067))->q_kids)->len)) {
            f_check__need_bool(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1068))->q_kids, v_i, sizeof(tb_o_Node *), 1068)));
            f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1069))->q_kids, (v_i + 1LL), sizeof(tb_o_Node *), 1069)));
            { long long tb_v497 = 2LL; long long *tb_p497 = &(v_i); *tb_p497 += tb_v497; }
        }
        if ((tb_mod((((tb_o_Node *)tb_nn(v_n, 1071))->q_kids)->len, 2LL, 1071) == 1LL)) {
            f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1072))->q_kids, ((((tb_o_Node *)tb_nn(v_n, 1072))->q_kids)->len - 1LL), sizeof(tb_o_Node *), 1072)));
        }
    } else if (({ const char *tb_s498 = v_k; ((unsigned char)tb_s498[0] == 101 && (unsigned char)tb_s498[1] == 97 && (unsigned char)tb_s498[2] == 99 && (unsigned char)tb_s498[3] == 104 && tb_s498[4] == 0); })) {
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
            { tb_o_Node * tb_v499 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1091))->q_loops) = tb_v499; }
            f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1092))->q_kids, 1LL, sizeof(tb_o_Node *), 1092)));
            tb_o_Node * v_gone = (*(tb_o_Node * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 1093))->q_loops, 1093));
            f_check__close_scope(v_c);
        }
    } else if (({ const char *tb_s500 = v_k; ((unsigned char)tb_s500[0] == 99 && (unsigned char)tb_s500[1] == 111 && (unsigned char)tb_s500[2] == 117 && (unsigned char)tb_s500[3] == 110 && (unsigned char)tb_s500[4] == 116 && tb_s500[5] == 0); })) {
        f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1096))->q_kids, 0LL, sizeof(tb_o_Node *), 1096)), "from");
        f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1097))->q_kids, 1LL, sizeof(tb_o_Node *), 1097)), "to");
        f_check__open_scope(v_c);
        f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1099))->q_name, 1LL, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1099))->q_line);
        { tb_o_Node * tb_v501 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1100))->q_loops) = tb_v501; }
        f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1101))->q_kids, 2LL, sizeof(tb_o_Node *), 1101)));
        tb_o_Node * v_gone = (*(tb_o_Node * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 1102))->q_loops, 1102));
        f_check__close_scope(v_c);
    } else if (((strcmp(v_k, "repeat_times") == 0) || (strcmp(v_k, "repeat_while") == 0))) {
        if ((strcmp(v_k, "repeat_times") == 0)) {
            f_check__need_int(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1106))->q_kids, 0LL, sizeof(tb_o_Node *), 1106)), "times");
        } else {
            f_check__need_bool(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1108))->q_kids, 0LL, sizeof(tb_o_Node *), 1108)));
        }
        { long long tb_v502 = 1LL; long long *tb_p502 = &(((tb_o_Comp *)tb_nn(v_c, 1109))->q_loop_depth); *tb_p502 += tb_v502; }
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1110))->q_kids, 1LL, sizeof(tb_o_Node *), 1110)));
        { long long tb_v503 = 1LL; long long *tb_p503 = &(((tb_o_Comp *)tb_nn(v_c, 1111))->q_loop_depth); *tb_p503 -= tb_v503; }
    } else if (({ const char *tb_s504 = v_k; ((unsigned char)tb_s504[0] == 116 && (unsigned char)tb_s504[1] == 111 && (unsigned char)tb_s504[2] == 103 && (unsigned char)tb_s504[3] == 101 && (unsigned char)tb_s504[4] == 116 && (unsigned char)tb_s504[5] == 104 && (unsigned char)tb_s504[6] == 101 && (unsigned char)tb_s504[7] == 114 && tb_s504[8] == 0); })) {
        f_check__check_together(v_c, v_n);
    } else if (({ const char *tb_s505 = v_k; ((unsigned char)tb_s505[0] == 116 && (unsigned char)tb_s505[1] == 101 && (unsigned char)tb_s505[2] == 109 && (unsigned char)tb_s505[3] == 112 && tb_s505[4] == 0); })) {
        long long v_saved_base = ((tb_o_Comp *)tb_nn(v_c, 1115))->q_temp_base;
        long long v_saved_loops = ((tb_o_Comp *)tb_nn(v_c, 1116))->q_temp_loops;
        { long long tb_v506 = (((tb_o_Comp *)tb_nn(v_c, 1117))->q_vars)->len; long long *tb_p506 = &(((tb_o_Comp *)tb_nn(v_c, 1117))->q_temp_base); *tb_p506 = tb_v506; }
        { long long tb_v507 = ((tb_o_Comp *)tb_nn(v_c, 1118))->q_loop_depth; long long *tb_p507 = &(((tb_o_Comp *)tb_nn(v_c, 1118))->q_temp_loops); *tb_p507 = tb_v507; }
        f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1119))->q_kids, 0LL, sizeof(tb_o_Node *), 1119)));
        { long long tb_v508 = v_saved_base; long long *tb_p508 = &(((tb_o_Comp *)tb_nn(v_c, 1120))->q_temp_base); *tb_p508 = tb_v508; }
        { long long tb_v509 = v_saved_loops; long long *tb_p509 = &(((tb_o_Comp *)tb_nn(v_c, 1121))->q_temp_loops); *tb_p509 = tb_v509; }
    } else if ((({ const char *tb_s510 = v_k; ((unsigned char)tb_s510[0] == 115 && (unsigned char)tb_s510[1] == 107 && (unsigned char)tb_s510[2] == 105 && (unsigned char)tb_s510[3] == 112 && tb_s510[4] == 0); }) || ({ const char *tb_s511 = v_k; ((unsigned char)tb_s511[0] == 115 && (unsigned char)tb_s511[1] == 116 && (unsigned char)tb_s511[2] == 111 && (unsigned char)tb_s511[3] == 112 && tb_s511[4] == 0); }))) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1123))->q_loop_depth == 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1124))->q_line, "skip и stop пишутся только внутри цикла", "");
        }
        if (((({ const char *tb_s512 = v_k; ((unsigned char)tb_s512[0] == 115 && (unsigned char)tb_s512[1] == 116 && (unsigned char)tb_s512[2] == 111 && (unsigned char)tb_s512[3] == 112 && tb_s512[4] == 0); }) && (((tb_o_Comp *)tb_nn(v_c, 1125))->q_par_base >= 0LL)) && (((tb_o_Comp *)tb_nn(v_c, 1125))->q_loop_depth == ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1125))->q_par_node, 1125))->q_temp_no))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1126))->q_line, "stop внутри on all cores нельзя: остальные ядра всё равно работают", "");
        }
        if (((((tb_o_Comp *)tb_nn(v_c, 1127))->q_temp_base >= 0LL) && (((tb_o_Comp *)tb_nn(v_c, 1127))->q_loop_depth == ((tb_o_Comp *)tb_nn(v_c, 1127))->q_temp_loops))) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1128))->q_line, "skip и stop внутри temp работают только для циклов внутри temp", "");
        }
    } else if (({ const char *tb_s513 = v_k; ((unsigned char)tb_s513[0] == 114 && (unsigned char)tb_s513[1] == 101 && (unsigned char)tb_s513[2] == 116 && (unsigned char)tb_s513[3] == 117 && (unsigned char)tb_s513[4] == 114 && (unsigned char)tb_s513[5] == 110 && tb_s513[6] == 0); })) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1130))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1131))->q_line, ({ const char *tb_c514_0 = "внутри "; long long tb_c514_0n = sizeof("внутри ") - 1; const char *tb_c514_1 = f_check__loop_word(v_c); long long tb_c514_1n = (long long)strlen(tb_c514_1); const char *tb_c514_2 = " return нельзя"; long long tb_c514_2n = sizeof(" return нельзя") - 1; char *tb_c514r = tb_mem(tb_c514_0n + tb_c514_1n + tb_c514_2n + 1), *tb_c514rw = tb_c514r; memcpy(tb_c514rw, tb_c514_0, tb_c514_0n); tb_c514rw += tb_c514_0n; memcpy(tb_c514rw, tb_c514_1, tb_c514_1n); tb_c514rw += tb_c514_1n; memcpy(tb_c514rw, tb_c514_2, tb_c514_2n); tb_c514rw += tb_c514_2n; *tb_c514rw = 0; (const char *)tb_c514r; }), "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1132))->q_temp_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1133))->q_line, "внутри temp return нельзя: сохрани число в переменную снаружи и выйди из блока", "");
        }
        if ((((tb_o_Comp *)tb_nn(v_c, 1134))->q_piece_depth > 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1135))->q_line, "внутри each ... by N return нельзя: сохрани число в переменную снаружи и выйди через stop", "");
        }
        f_check__check_return(v_c, v_n);
    } else if (({ const char *tb_s515 = v_k; ((unsigned char)tb_s515[0] == 102 && (unsigned char)tb_s515[1] == 97 && (unsigned char)tb_s515[2] == 105 && (unsigned char)tb_s515[3] == 108 && tb_s515[4] == 0); })) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1138))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1139))->q_line, ({ const char *tb_c516_0 = "внутри "; long long tb_c516_0n = sizeof("внутри ") - 1; const char *tb_c516_1 = f_check__loop_word(v_c); long long tb_c516_1n = (long long)strlen(tb_c516_1); const char *tb_c516_2 = " fail нельзя: пиши  or  или  else err"; long long tb_c516_2n = sizeof(" fail нельзя: пиши  or  или  else err") - 1; char *tb_c516r = tb_mem(tb_c516_0n + tb_c516_1n + tb_c516_2n + 1), *tb_c516rw = tb_c516r; memcpy(tb_c516rw, tb_c516_0, tb_c516_0n); tb_c516rw += tb_c516_0n; memcpy(tb_c516rw, tb_c516_1, tb_c516_1n); tb_c516rw += tb_c516_1n; memcpy(tb_c516rw, tb_c516_2, tb_c516_2n); tb_c516rw += tb_c516_2n; *tb_c516rw = 0; (const char *)tb_c516r; }), "");
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
        if ((({ const char *tb_s517 = ((tb_o_Node *)tb_nn(v_e, 1149))->q_kind; ((unsigned char)tb_s517[0] == 116 && (unsigned char)tb_s517[1] == 114 && (unsigned char)tb_s517[2] == 121 && tb_s517[3] == 0); }) || ({ const char *tb_s518 = ((tb_o_Node *)tb_nn(v_e, 1149))->q_kind; ((unsigned char)tb_s518[0] == 101 && (unsigned char)tb_s518[1] == 108 && (unsigned char)tb_s518[2] == 115 && (unsigned char)tb_s518[3] == 101 && (unsigned char)tb_s518[4] == 95 && (unsigned char)tb_s518[5] == 101 && (unsigned char)tb_s518[6] == 114 && (unsigned char)tb_s518[7] == 114 && tb_s518[8] == 0); }))) {
            v_inner = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1150))->q_kids, 0LL, sizeof(tb_o_Node *), 1150));
        }
        if (((({ const char *tb_s519 = ((tb_o_Node *)tb_nn(v_inner, 1151))->q_kind; !((unsigned char)tb_s519[0] == 99 && (unsigned char)tb_s519[1] == 97 && (unsigned char)tb_s519[2] == 108 && (unsigned char)tb_s519[3] == 108 && tb_s519[4] == 0); }) && ({ const char *tb_s520 = ((tb_o_Node *)tb_nn(v_inner, 1151))->q_kind; !((unsigned char)tb_s520[0] == 109 && (unsigned char)tb_s520[1] == 101 && (unsigned char)tb_s520[2] == 116 && (unsigned char)tb_s520[3] == 104 && (unsigned char)tb_s520[4] == 111 && (unsigned char)tb_s520[5] == 100 && tb_s520[6] == 0); })) && (!(({ const char *tb_s521 = ((tb_o_Node *)tb_nn(v_inner, 1151))->q_kind; ((unsigned char)tb_s521[0] == 102 && (unsigned char)tb_s521[1] == 105 && (unsigned char)tb_s521[2] == 101 && (unsigned char)tb_s521[3] == 108 && (unsigned char)tb_s521[4] == 100 && tb_s521[5] == 0); }) && ({ const char *tb_s522 = ((tb_o_Node *)tb_nn(v_inner, 1151))->q_name; ((unsigned char)tb_s522[0] == 119 && (unsigned char)tb_s522[1] == 97 && (unsigned char)tb_s522[2] == 105 && (unsigned char)tb_s522[3] == 116 && tb_s522[4] == 0); }))))) {
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
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1162))->q_line, ({ const char *tb_c523_0 = "внутри "; long long tb_c523_0n = sizeof("внутри ") - 1; const char *tb_c523_1 = f_check__loop_word(v_c); long long tb_c523_1n = (long long)strlen(tb_c523_1); const char *tb_c523_2 = " background нельзя: цикл и так идёт на всех ядрах"; long long tb_c523_2n = sizeof(" background нельзя: цикл и так идёт на всех ядрах") - 1; char *tb_c523r = tb_mem(tb_c523_0n + tb_c523_1n + tb_c523_2n + 1), *tb_c523rw = tb_c523r; memcpy(tb_c523rw, tb_c523_0, tb_c523_0n); tb_c523rw += tb_c523_0n; memcpy(tb_c523rw, tb_c523_1, tb_c523_1n); tb_c523rw += tb_c523_1n; memcpy(tb_c523rw, tb_c523_2, tb_c523_2n); tb_c523rw += tb_c523_2n; *tb_c523rw = 0; (const char *)tb_c523r; }), "");
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1163))->q_temp_base >= 0LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1164))->q_line, "внутри temp background нельзя: память блока удалится, пока функция ещё работает", "");
    }
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1165))->q_kids, 0LL, sizeof(tb_o_Node *), 1165));
    if ((({ const char *tb_s524 = ((tb_o_Node *)tb_nn(v_call, 1166))->q_kind; ((unsigned char)tb_s524[0] == 99 && (unsigned char)tb_s524[1] == 97 && (unsigned char)tb_s524[2] == 108 && (unsigned char)tb_s524[3] == 108 && tb_s524[4] == 0); }) && f_check__is_builtin(((tb_o_Node *)tb_nn(v_call, 1166))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1167))->q_line, "background запускает свою функцию: a = background load(name)", ((tb_o_Node *)tb_nn(v_call, 1167))->q_name);
    }
    { _Bool tb_v525 = 1; _Bool *tb_p525 = &(((tb_o_Node *)tb_nn(v_call, 1168))->q_creates); *tb_p525 = tb_v525; }
    long long v_t = f_check__check_expr(v_c, v_call);
    if (((({ const char *tb_s526 = ((tb_o_Node *)tb_nn(v_call, 1170))->q_kind; !((unsigned char)tb_s526[0] == 99 && (unsigned char)tb_s526[1] == 97 && (unsigned char)tb_s526[2] == 108 && (unsigned char)tb_s526[3] == 108 && tb_s526[4] == 0); }) || (!((tb_o_Node *)tb_nn(v_call, 1170))->q_has_ref)) || ({ const char *tb_s527 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_call, 1170))->q_ref, 1170))->q_kind; !((unsigned char)tb_s527[0] == 102 && (unsigned char)tb_s527[1] == 117 && (unsigned char)tb_s527[2] == 110 && (unsigned char)tb_s527[3] == 99 && tb_s527[4] == 0); }))) {
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
        tb_list * tb_l528 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1177))->q_kids, 0LL, sizeof(tb_o_Node *), 1177)), 1177))->q_kids;
        for (long long tb_i528 = 0; tb_i528 < tb_l528->len; tb_i528++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l528->data)[tb_i528];
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
        tb_list * tb_l529 = ((tb_o_Node *)tb_nn(v_block, 1189))->q_kids;
        for (long long tb_i529 = 0; tb_i529 < tb_l529->len; tb_i529++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l529->data)[tb_i529];
            if ((({ const char *tb_s530 = ((tb_o_Node *)tb_nn(v_s, 1190))->q_kind; !((unsigned char)tb_s530[0] == 97 && (unsigned char)tb_s530[1] == 115 && (unsigned char)tb_s530[2] == 115 && (unsigned char)tb_s530[3] == 105 && (unsigned char)tb_s530[4] == 103 && (unsigned char)tb_s530[5] == 110 && tb_s530[6] == 0); }) || ((((tb_o_Node *)tb_nn(v_s, 1190))->q_kids)->len != 1LL))) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1191))->q_line, "в together пишутся только новые переменные со значением: a = d[key]", "");
            }
            if ((f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_s, 1192))->q_name) >= 0LL)) {
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1193))->q_line, "в together заводятся только новые переменные, а эта уже есть", ((tb_o_Node *)tb_nn(v_s, 1193))->q_name);
            }
            {
                tb_list * tb_l531 = v_names;
                for (long long tb_i531 = 0; tb_i531 < tb_l531->len; tb_i531++) {
                    const char * const v_w = ((const char * *)tb_l531->data)[tb_i531];
                    if ((strcmp(v_w, ((tb_o_Node *)tb_nn(v_s, 1195))->q_name) == 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1196))->q_line, "в together эта переменная уже заведена строкой выше", ((tb_o_Node *)tb_nn(v_s, 1196))->q_name);
                    }
                    if (f_check__uses_name((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_s, 1197))->q_kids, 0LL, sizeof(tb_o_Node *), 1197)), v_w)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_s, 1198))->q_line, "строки в together не должны зависеть друг от друга, а здесь нужна", v_w);
                    }
                }
            }
            { const char * tb_v532 = ((tb_o_Node *)tb_nn(v_s, 1199))->q_name; *(const char * *)tb_grow(v_names) = tb_v532; }
            f_check__check_statement(v_c, v_s);
        }
    }
}

static _Bool f_check__uses_name(tb_o_Node * v_n, const char * v_name) {
    if ((({ const char *tb_s533 = ((tb_o_Node *)tb_nn(v_n, 1204))->q_kind; ((unsigned char)tb_s533[0] == 110 && (unsigned char)tb_s533[1] == 97 && (unsigned char)tb_s533[2] == 109 && (unsigned char)tb_s533[3] == 101 && tb_s533[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1204))->q_name, v_name) == 0))) {
        return 1;
    }
    {
        tb_list * tb_l534 = ((tb_o_Node *)tb_nn(v_n, 1206))->q_kids;
        for (long long tb_i534 = 0; tb_i534 < tb_l534->len; tb_i534++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l534->data)[tb_i534];
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
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1221))->q_line, ({ const char *tb_c535_0 = "each ... by N внутри "; long long tb_c535_0n = sizeof("each ... by N внутри ") - 1; const char *tb_c535_1 = f_check__loop_word(v_c); long long tb_c535_1n = (long long)strlen(tb_c535_1); const char *tb_c535_2 = " пока нельзя"; long long tb_c535_2n = sizeof(" пока нельзя") - 1; char *tb_c535r = tb_mem(tb_c535_0n + tb_c535_1n + tb_c535_2n + 1), *tb_c535rw = tb_c535r; memcpy(tb_c535rw, tb_c535_0, tb_c535_0n); tb_c535rw += tb_c535_0n; memcpy(tb_c535rw, tb_c535_1, tb_c535_1n); tb_c535rw += tb_c535_1n; memcpy(tb_c535rw, tb_c535_2, tb_c535_2n); tb_c535rw += tb_c535_2n; *tb_c535rw = 0; (const char *)tb_c535r; }), "");
    }
    f_check__need_int(v_c, ((tb_o_Node *)tb_nn(v_n, 1222))->q_piece_size, "by");
    { const char * tb_v536 = ((tb_o_Comp *)tb_nn(v_c, 1223))->q_file_name; const char * *tb_p536 = &(((tb_o_Node *)tb_nn(v_n, 1223))->q_file); *tb_p536 = tb_v536; }
    { tb_o_Node * tb_v537 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1224))->q_pieces) = tb_v537; }
    f_check__open_scope(v_c);
    f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_n, 1226))->q_name, v_t, 1, 0, ((tb_o_Node *)tb_nn(v_n, 1226))->q_line);
    { long long tb_v538 = 1LL; long long *tb_p538 = &(((tb_o_Comp *)tb_nn(v_c, 1227))->q_piece_depth); *tb_p538 += tb_v538; }
    { tb_o_Node * tb_v539 = v_n; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1228))->q_loops) = tb_v539; }
    f_check__check_loop_body(v_c, v_n, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1229))->q_kids, 1LL, sizeof(tb_o_Node *), 1229)));
    tb_o_Node * v_gone = (*(tb_o_Node * *)tb_pop(((tb_o_Comp *)tb_nn(v_c, 1230))->q_loops, 1230));
    { long long tb_v540 = 1LL; long long *tb_p540 = &(((tb_o_Comp *)tb_nn(v_c, 1231))->q_piece_depth); *tb_p540 -= tb_v540; }
    f_check__close_scope(v_c);
}

static _Bool f_check__is_name_of(tb_o_Node * v_v, const char * v_name) {
    return (({ const char *tb_s541 = ((tb_o_Node *)tb_nn(v_v, 1236))->q_kind; ((unsigned char)tb_s541[0] == 110 && (unsigned char)tb_s541[1] == 97 && (unsigned char)tb_s541[2] == 109 && (unsigned char)tb_s541[3] == 101 && tb_s541[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_v, 1236))->q_name, v_name) == 0));
    tb_crash(1235, "функция is_name_of дошла до конца без return");
    return 0;
}

static long long f_check__piece_leak(tb_o_Comp * v_c, tb_o_Node * v_n, const char * v_name) {
    long long v_r = 0LL;
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1242))->q_kind;
    if ((({ const char *tb_s542 = v_k; ((unsigned char)tb_s542[0] == 110 && (unsigned char)tb_s542[1] == 97 && (unsigned char)tb_s542[2] == 109 && (unsigned char)tb_s542[3] == 101 && tb_s542[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1243))->q_name, v_name) == 0))) {
        return ((tb_o_Node *)tb_nn(v_n, 1244))->q_line;
    }
    if (((({ const char *tb_s543 = v_k; ((unsigned char)tb_s543[0] == 97 && (unsigned char)tb_s543[1] == 115 && (unsigned char)tb_s543[2] == 115 && (unsigned char)tb_s543[3] == 105 && (unsigned char)tb_s543[4] == 103 && (unsigned char)tb_s543[5] == 110 && tb_s543[6] == 0); }) || ({ const char *tb_s544 = v_k; ((unsigned char)tb_s544[0] == 108 && (unsigned char)tb_s544[1] == 105 && (unsigned char)tb_s544[2] == 110 && (unsigned char)tb_s544[3] == 107 && (unsigned char)tb_s544[4] == 100 && (unsigned char)tb_s544[5] == 101 && (unsigned char)tb_s544[6] == 99 && (unsigned char)tb_s544[7] == 108 && tb_s544[8] == 0); })) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1245))->q_name, v_name) == 0))) {
        return ((tb_o_Node *)tb_nn(v_n, 1246))->q_line;
    }
    if (((({ const char *tb_s545 = v_k; ((unsigned char)tb_s545[0] == 99 && (unsigned char)tb_s545[1] == 97 && (unsigned char)tb_s545[2] == 108 && (unsigned char)tb_s545[3] == 108 && tb_s545[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1247))->q_has_ref) && ({ const char *tb_s546 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1247))->q_ref, 1247))->q_kind; ((unsigned char)tb_s546[0] == 102 && (unsigned char)tb_s546[1] == 117 && (unsigned char)tb_s546[2] == 110 && (unsigned char)tb_s546[3] == 99 && tb_s546[4] == 0); }))) {
        {
            long long tb_end547 = ((((tb_o_Node *)tb_nn(v_n, 1248))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end547; v_i++) {
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
    if ((({ const char *tb_s548 = v_k; ((unsigned char)tb_s548[0] == 99 && (unsigned char)tb_s548[1] == 97 && (unsigned char)tb_s548[2] == 108 && (unsigned char)tb_s548[3] == 108 && tb_s548[4] == 0); }) && f_check__is_builtin(((tb_o_Node *)tb_nn(v_n, 1258))->q_name))) {
        {
            tb_list * tb_l549 = ((tb_o_Node *)tb_nn(v_n, 1259))->q_kids;
            for (long long tb_i549 = 0; tb_i549 < tb_l549->len; tb_i549++) {
                tb_o_Node * const v_a = ((tb_o_Node * *)tb_l549->data)[tb_i549];
                tb_o_Node * v_v = v_a;
                if (({ const char *tb_s550 = ((tb_o_Node *)tb_nn(v_a, 1261))->q_kind; ((unsigned char)tb_s550[0] == 97 && (unsigned char)tb_s550[1] == 114 && (unsigned char)tb_s550[2] == 103 && tb_s550[3] == 0); })) {
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
    if ((((({ const char *tb_s551 = v_k; ((unsigned char)tb_s551[0] == 105 && (unsigned char)tb_s551[1] == 110 && (unsigned char)tb_s551[2] == 100 && (unsigned char)tb_s551[3] == 101 && (unsigned char)tb_s551[4] == 120 && tb_s551[5] == 0); }) || ({ const char *tb_s552 = v_k; ((unsigned char)tb_s552[0] == 101 && (unsigned char)tb_s552[1] == 97 && (unsigned char)tb_s552[2] == 99 && (unsigned char)tb_s552[3] == 104 && tb_s552[4] == 0); })) && ((((tb_o_Node *)tb_nn(v_n, 1269))->q_kids)->len > 0LL)) && f_check__is_name_of((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1269))->q_kids, 0LL, sizeof(tb_o_Node *), 1269)), v_name))) {
        v_start = 1LL;
    }
    if ((({ const char *tb_s553 = v_k; ((unsigned char)tb_s553[0] == 101 && (unsigned char)tb_s553[1] == 97 && (unsigned char)tb_s553[2] == 99 && (unsigned char)tb_s553[3] == 104 && tb_s553[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1271))->q_by_piece)) {
        long long v_bad = f_check__piece_leak(v_c, ((tb_o_Node *)tb_nn(v_n, 1272))->q_piece_size, v_name);
        if ((v_bad != 0LL)) {
            return v_bad;
        }
    }
    {
        long long tb_end554 = ((((tb_o_Node *)tb_nn(v_n, 1275))->q_kids)->len - 1LL);
        for (long long v_i = v_start; v_i <= tb_end554; v_i++) {
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
    const char * v_key = ({ const char *tb_c555_0 = ((tb_o_Node *)tb_nn(v_f, 1283))->q_name; long long tb_c555_0n = (long long)strlen(tb_c555_0); const char *tb_c555_1 = "@"; long long tb_c555_1n = sizeof("@") - 1; const char *tb_c555_2 = ((tb_o_Node *)tb_nn(v_f, 1283))->q_mod; long long tb_c555_2n = (long long)strlen(tb_c555_2); const char *tb_c555_3 = "#"; long long tb_c555_3n = sizeof("#") - 1; char tb_c555_4d[24]; long long tb_c555_4n; const char *tb_c555_4 = tb_digits(v_i, tb_c555_4d, &tb_c555_4n); char *tb_c555r = tb_mem(tb_c555_0n + tb_c555_1n + tb_c555_2n + tb_c555_3n + tb_c555_4n + 1), *tb_c555rw = tb_c555r; memcpy(tb_c555rw, tb_c555_0, tb_c555_0n); tb_c555rw += tb_c555_0n; memcpy(tb_c555rw, tb_c555_1, tb_c555_1n); tb_c555rw += tb_c555_1n; memcpy(tb_c555rw, tb_c555_2, tb_c555_2n); tb_c555rw += tb_c555_2n; memcpy(tb_c555rw, tb_c555_3, tb_c555_3n); tb_c555rw += tb_c555_3n; memcpy(tb_c555rw, tb_c555_4, tb_c555_4n); tb_c555rw += tb_c555_4n; *tb_c555rw = 0; (const char *)tb_c555r; });
    {
        long long tb_end556 = ((((tb_o_Comp *)tb_nn(v_c, 1284))->q_safe_keys)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end556; v_j++) {
            if ((strcmp((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1285))->q_safe_keys, v_j, sizeof(const char *), 1285)), v_key) == 0)) {
                return ((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1286))->q_safe_vals, v_j, sizeof(long long), 1286)) == 2LL);
            }
        }
    }
    { const char * tb_v557 = v_key; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1288))->q_safe_keys) = tb_v557; }
    { long long tb_v558 = 1LL; *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1289))->q_safe_vals) = tb_v558; }
    long long v_slot = ((((tb_o_Comp *)tb_nn(v_c, 1290))->q_safe_keys)->len - 1LL);
    { long long tb_v559 = 1LL; long long *tb_p559 = &(((tb_o_Comp *)tb_nn(v_c, 1291))->q_safe_busy); *tb_p559 += tb_v559; }
    long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1292))->q_kids, 1LL, sizeof(tb_o_Node *), 1292)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1292))->q_kids, 0LL, sizeof(tb_o_Node *), 1292)), 1292))->q_kids, v_i, sizeof(tb_o_Node *), 1292)), 1292))->q_name);
    { long long tb_v560 = 1LL; long long *tb_p560 = &(((tb_o_Comp *)tb_nn(v_c, 1293))->q_safe_busy); *tb_p560 -= tb_v560; }
    if ((v_bad != 0LL)) {
        { long long tb_v561 = 2LL; long long *tb_p561 = &((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1295))->q_safe_vals, v_slot, sizeof(long long), 1295))); *tb_p561 = tb_v561; }
        return 1;
    }
    if ((((tb_o_Comp *)tb_nn(v_c, 1297))->q_safe_busy > 0LL)) {
        { long long tb_v562 = 3LL; long long *tb_p562 = &((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1299))->q_safe_vals, v_slot, sizeof(long long), 1299))); *tb_p562 = tb_v562; }
        { const char * tb_v563 = ""; const char * *tb_p563 = &((*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1300))->q_safe_keys, v_slot, sizeof(const char *), 1300))); *tb_p563 = tb_v563; }
    }
    return 0;
    tb_crash(1282, "функция param_keeps_piece дошла до конца без return");
    return 0;
}

static void f_check__mark_hot_fields(tb_o_Comp * v_c) {
    tb_list * v_hot_func = tb_bits_new();
    {
        long long tb_end564 = (((tb_o_Comp *)tb_nn(v_c, 1310))->q_funcs)->len;
        for (long long v_i = 1LL; v_i <= tb_end564; v_i++) {
            { _Bool tb_v565 = 0; tb_badd(v_hot_func, tb_v565); }
        }
    }
    {
        long long tb_end566 = ((((tb_o_Comp *)tb_nn(v_c, 1312))->q_call_to)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end566; v_j++) {
            if (tb_bget(((tb_o_Comp *)tb_nn(v_c, 1313))->q_call_loop, v_j, 1313)) {
                { _Bool tb_v567 = 1; tb_bset(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1314))->q_call_to, v_j, sizeof(long long), 1314)), tb_v567, 1314); }
            }
        }
    }
    _Bool v_changed = 1;
    while (v_changed) {
        v_changed = 0;
        {
            long long tb_end568 = ((((tb_o_Comp *)tb_nn(v_c, 1318))->q_call_to)->len - 1LL);
            for (long long v_j = 0LL; v_j <= tb_end568; v_j++) {
                if (((((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1319))->q_call_from, v_j, sizeof(long long), 1319)) >= 0LL) && tb_bget(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1319))->q_call_from, v_j, sizeof(long long), 1319)), 1319)) && (!tb_bget(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1319))->q_call_to, v_j, sizeof(long long), 1319)), 1319)))) {
                    { _Bool tb_v569 = 1; tb_bset(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1320))->q_call_to, v_j, sizeof(long long), 1320)), tb_v569, 1320); }
                    v_changed = 1;
                }
            }
        }
    }
    {
        long long tb_end570 = ((((tb_o_Comp *)tb_nn(v_c, 1322))->q_acc_field)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end570; v_j++) {
            if ((tb_bget(((tb_o_Comp *)tb_nn(v_c, 1323))->q_acc_loop, v_j, 1323) || (((*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1323))->q_acc_func, v_j, sizeof(long long), 1323)) >= 0LL) && tb_bget(v_hot_func, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1323))->q_acc_func, v_j, sizeof(long long), 1323)), 1323)))) {
                { _Bool tb_v571 = 1; _Bool *tb_p571 = &(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1324))->q_acc_field, v_j, sizeof(tb_o_Node *), 1324)), 1324))->q_hot); *tb_p571 = tb_v571; }
            }
        }
    }
    {
        tb_list * tb_l572 = ((tb_o_Comp *)tb_nn(v_c, 1325))->q_objs;
        for (long long tb_i572 = 0; tb_i572 < tb_l572->len; tb_i572++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l572->data)[tb_i572];
            if (((tb_o_Node *)tb_nn(v_o, 1326))->q_by_cols) {
                continue;
            }
            long long v_hot = 0LL;
            long long v_cold_bytes = 0LL;
            {
                tb_list * tb_l573 = ((tb_o_Node *)tb_nn(v_o, 1330))->q_kids;
                for (long long tb_i573 = 0; tb_i573 < tb_l573->len; tb_i573++) {
                    tb_o_Node * const v_f = ((tb_o_Node * *)tb_l573->data)[tb_i573];
                    if (((tb_o_Node *)tb_nn(v_f, 1331))->q_hot) {
                        { long long tb_v574 = 1LL; long long *tb_p574 = &(v_hot); *tb_p574 += tb_v574; }
                    } else if ((((tb_o_Node *)tb_nn(v_f, 1333))->q_ty == 3LL)) {
                        { long long tb_v575 = 1LL; long long *tb_p575 = &(v_cold_bytes); *tb_p575 += tb_v575; }
                    } else {
                        { long long tb_v576 = 8LL; long long *tb_p576 = &(v_cold_bytes); *tb_p576 += tb_v576; }
                    }
                }
            }
            if (((v_hot > 0LL) && (v_cold_bytes >= 16LL))) {
                {
                    tb_list * tb_l577 = ((tb_o_Node *)tb_nn(v_o, 1338))->q_kids;
                    for (long long tb_i577 = 0; tb_i577 < tb_l577->len; tb_i577++) {
                        tb_o_Node * const v_f = ((tb_o_Node * *)tb_l577->data)[tb_i577];
                        { _Bool tb_v578 = (!((tb_o_Node *)tb_nn(v_f, 1339))->q_hot); _Bool *tb_p578 = &(((tb_o_Node *)tb_nn(v_f, 1339))->q_is_cold); *tb_p578 = tb_v578; }
                    }
                }
            }
        }
    }
}

static void f_check__check_piece_leaks(tb_o_Comp * v_c) {
    {
        tb_list * tb_l579 = ((tb_o_Comp *)tb_nn(v_c, 1343))->q_pieces;
        for (long long tb_i579 = 0; tb_i579 < tb_l579->len; tb_i579++) {
            tb_o_Node * const v_n = ((tb_o_Node * *)tb_l579->data)[tb_i579];
            long long v_bad = f_check__piece_leak(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1344))->q_kids, 1LL, sizeof(tb_o_Node *), 1344)), ((tb_o_Node *)tb_nn(v_n, 1344))->q_name);
            if ((v_bad != 0LL)) {
                { const char * tb_v580 = ((tb_o_Node *)tb_nn(v_n, 1346))->q_file; const char * *tb_p580 = &(((tb_o_Comp *)tb_nn(v_c, 1346))->q_file_name); *tb_p580 = tb_v580; }
                f_errors__error_at(v_c, v_bad, "кусок живёт только внутри своего цикла: его можно читать и менять по номеру, ходить по нему each и передавать в функцию, но нельзя сохранять, растить .add и уменьшать .pop", ((tb_o_Node *)tb_nn(v_n, 1347))->q_name);
            }
        }
    }
}

static void f_check__check_loop_body(tb_o_Comp * v_c, tb_o_Node * v_n, tb_o_Node * v_body) {
    { long long tb_v581 = 1LL; long long *tb_p581 = &(((tb_o_Comp *)tb_nn(v_c, 1352))->q_loop_depth); *tb_p581 += tb_v581; }
    if ((((tb_o_Node *)tb_nn(v_n, 1353))->q_is_par || ((tb_o_Node *)tb_nn(v_n, 1353))->q_by_vec)) {
        if ((((tb_o_Comp *)tb_nn(v_c, 1354))->q_par_base >= 0LL)) {
            f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_n, 1355))->q_line, "on all cores и by vec внутри другого on all cores или by vec нельзя", "");
        }
        { long long tb_v582 = ((((tb_o_Comp *)tb_nn(v_c, 1356))->q_vars)->len - 1LL); long long *tb_p582 = &(((tb_o_Comp *)tb_nn(v_c, 1356))->q_par_base); *tb_p582 = tb_v582; }
        { tb_o_Node * tb_v583 = v_n; tb_o_Node * *tb_p583 = &(((tb_o_Comp *)tb_nn(v_c, 1357))->q_par_node); *tb_p583 = tb_v583; }
        { _Bool tb_v584 = ((tb_o_Node *)tb_nn(v_n, 1358))->q_by_vec; _Bool *tb_p584 = &(((tb_o_Comp *)tb_nn(v_c, 1358))->q_par_vec); *tb_p584 = tb_v584; }
        { long long tb_v585 = ((tb_o_Comp *)tb_nn(v_c, 1360))->q_loop_depth; long long *tb_p585 = &(((tb_o_Node *)tb_nn(v_n, 1360))->q_temp_no); *tb_p585 = tb_v585; }
        f_check__check_block(v_c, v_body);
        { long long tb_v586 = (-1LL); long long *tb_p586 = &(((tb_o_Comp *)tb_nn(v_c, 1362))->q_par_base); *tb_p586 = tb_v586; }
        { _Bool tb_v587 = 0; _Bool *tb_p587 = &(((tb_o_Comp *)tb_nn(v_c, 1363))->q_par_vec); *tb_p587 = tb_v587; }
        if (((tb_o_Node *)tb_nn(v_n, 1364))->q_by_vec) {
            {
                tb_list * tb_l588 = ((tb_o_Node *)tb_nn(v_n, 1366))->q_caps;
                for (long long tb_i588 = 0; tb_i588 < tb_l588->len; tb_i588++) {
                    tb_o_Var * const v_a = ((tb_o_Var * *)tb_l588->data)[tb_i588];
                    {
                        tb_list * tb_l589 = ((tb_o_Node *)tb_nn(v_n, 1367))->q_caps;
                        for (long long tb_i589 = 0; tb_i589 < tb_l589->len; tb_i589++) {
                            tb_o_Var * const v_b = ((tb_o_Var * *)tb_l589->data)[tb_i589];
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
    { long long tb_v590 = 1LL; long long *tb_p590 = &(((tb_o_Comp *)tb_nn(v_c, 1372))->q_loop_depth); *tb_p590 -= tb_v590; }
}

static void f_check__check_block(tb_o_Comp * v_c, tb_o_Node * v_block) {
    f_check__open_scope(v_c);
    {
        tb_list * tb_l591 = ((tb_o_Node *)tb_nn(v_block, 1376))->q_kids;
        for (long long tb_i591 = 0; tb_i591 < tb_l591->len; tb_i591++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l591->data)[tb_i591];
            f_check__check_statement(v_c, v_s);
        }
    }
    f_check__close_scope(v_c);
}

static void f_check__check_func(tb_o_Comp * v_c, long long v_fi) {
    tb_o_Func * v_f = (*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1381))->q_funcs, v_fi, sizeof(tb_o_Func *), 1381));
    { long long tb_v592 = v_fi; long long *tb_p592 = &(((tb_o_Comp *)tb_nn(v_c, 1382))->q_current); *tb_p592 = tb_v592; }
    { const char * tb_v593 = ((tb_o_Func *)tb_nn(v_f, 1383))->q_mod; const char * *tb_p593 = &(((tb_o_Comp *)tb_nn(v_c, 1383))->q_cur_mod); *tb_p593 = tb_v593; }
    { const char * tb_v594 = ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1384))->q_node, 1384))->q_file; const char * *tb_p594 = &(((tb_o_Comp *)tb_nn(v_c, 1384))->q_file_name); *tb_p594 = tb_v594; }
    { long long tb_v595 = 1LL; long long *tb_p595 = &(((tb_o_Func *)tb_nn(v_f, 1385))->q_state); *tb_p595 = tb_v595; }
    { long long tb_v596 = 0LL; long long *tb_p596 = &(((tb_o_Comp *)tb_nn(v_c, 1386))->q_loop_depth); *tb_p596 = tb_v596; }
    f_check__open_scope(v_c);
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1388))->q_node, 1388))->q_kids, 0LL, sizeof(tb_o_Node *), 1388));
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1389))->q_node, 1389))->q_kids, 2LL, sizeof(tb_o_Node *), 1389));
    {
        tb_list * tb_l597 = ((tb_o_Node *)tb_nn(v_params, 1390))->q_kids;
        for (long long tb_i597 = 0; tb_i597 < tb_l597->len; tb_i597++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l597->data)[tb_i597];
            f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_p, 1391))->q_name, ((tb_o_Node *)tb_nn(v_p, 1391))->q_ty, 0, ((tb_o_Node *)tb_nn(v_p, 1391))->q_is_link, ((tb_o_Node *)tb_nn(v_p, 1391))->q_line);
        }
    }
    {
        tb_list * tb_l598 = ((tb_o_Node *)tb_nn(v_outputs, 1392))->q_kids;
        for (long long tb_i598 = 0; tb_i598 < tb_l598->len; tb_i598++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l598->data)[tb_i598];
            long long v_same_param = f_check__find_var(v_c, ((tb_o_Node *)tb_nn(v_o, 1393))->q_name);
            if ((v_same_param >= 0LL)) {
                if ((((tb_o_Var *)tb_nn((*(tb_o_Var * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1396))->q_vars, v_same_param, sizeof(tb_o_Var *), 1396)), 1396))->q_type != ((tb_o_Node *)tb_nn(v_o, 1396))->q_ty)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1397))->q_line, "выход и параметр с одним именем должны быть одного типа", ((tb_o_Node *)tb_nn(v_o, 1397))->q_name);
                }
                { _Bool tb_v599 = 1; _Bool *tb_p599 = &(((tb_o_Node *)tb_nn(v_o, 1399))->q_is_const); *tb_p599 = tb_v599; }
            } else {
                f_check__add_var(v_c, ((tb_o_Node *)tb_nn(v_o, 1401))->q_name, ((tb_o_Node *)tb_nn(v_o, 1401))->q_ty, 0, 0, ((tb_o_Node *)tb_nn(v_o, 1401))->q_line);
            }
        }
    }
    f_check__check_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1402))->q_node, 1402))->q_kids, 1LL, sizeof(tb_o_Node *), 1402)));
    f_check__close_scope(v_c);
    if ((((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1404))->q_node, 1404))->q_ty < 0LL)) {
        { long long tb_v600 = 0LL; long long *tb_p600 = &(((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1405))->q_node, 1405))->q_ty); *tb_p600 = tb_v600; }
    }
    { long long tb_v601 = 2LL; long long *tb_p601 = &(((tb_o_Func *)tb_nn(v_f, 1406))->q_state); *tb_p601 = tb_v601; }
}

static void f_check__register_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_o, 1409))->q_name) >= 0LL) || f_check__is_builtin(((tb_o_Node *)tb_nn(v_o, 1409))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1410))->q_line, "это имя уже занято", ((tb_o_Node *)tb_nn(v_o, 1410))->q_name);
    }
    if (((((tb_o_Comp *)tb_nn(v_c, 1411))->q_objs)->len == 500LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_o, 1412))->q_line, "слишком много obj", "");
    }
    { tb_o_Node * tb_v602 = v_o; *(tb_o_Node * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1413))->q_objs) = tb_v602; }
}

static void f_check__register_func(tb_o_Comp * v_c, tb_o_Node * v_fn) {
    if ((((f_check__find_func_in(v_c, ((tb_o_Node *)tb_nn(v_fn, 1416))->q_name, ((tb_o_Node *)tb_nn(v_fn, 1416))->q_mod) >= 0LL) || (f_check__find_obj(v_c, ((tb_o_Node *)tb_nn(v_fn, 1416))->q_name) >= 0LL)) || f_check__is_builtin(((tb_o_Node *)tb_nn(v_fn, 1416))->q_name))) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_fn, 1417))->q_line, "это имя уже занято", ((tb_o_Node *)tb_nn(v_fn, 1417))->q_name);
    }
    if (((((tb_o_Comp *)tb_nn(v_c, 1418))->q_funcs)->len == 1000LL)) {
        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_fn, 1419))->q_line, "слишком много функций", "");
    }
    { tb_o_Func * tb_v603 = tb_new_Func(((tb_o_Node *)tb_nn(v_fn, 1420))->q_name, ((tb_o_Node *)tb_nn(v_fn, 1420))->q_mod, v_fn, 0LL); *(tb_o_Func * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1420))->q_funcs) = tb_v603; }
}

static _Bool f_check__has_fail(tb_o_Node * v_n) {
    _Bool v_r = 0;
    if ((({ const char *tb_s604 = ((tb_o_Node *)tb_nn(v_n, 1424))->q_kind; ((unsigned char)tb_s604[0] == 102 && (unsigned char)tb_s604[1] == 97 && (unsigned char)tb_s604[2] == 105 && (unsigned char)tb_s604[3] == 108 && tb_s604[4] == 0); }) || ({ const char *tb_s605 = ((tb_o_Node *)tb_nn(v_n, 1424))->q_kind; ((unsigned char)tb_s605[0] == 116 && (unsigned char)tb_s605[1] == 114 && (unsigned char)tb_s605[2] == 121 && tb_s605[3] == 0); }))) {
        return 1;
    }
    {
        tb_list * tb_l606 = ((tb_o_Node *)tb_nn(v_n, 1426))->q_kids;
        for (long long tb_i606 = 0; tb_i606 < tb_l606->len; tb_i606++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l606->data)[tb_i606];
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
    if ((({ const char *tb_s607 = ((tb_o_Node *)tb_nn(v_n, 1433))->q_kind; ((unsigned char)tb_s607[0] == 114 && (unsigned char)tb_s607[1] == 101 && (unsigned char)tb_s607[2] == 116 && (unsigned char)tb_s607[3] == 117 && (unsigned char)tb_s607[4] == 114 && (unsigned char)tb_s607[5] == 110 && tb_s607[6] == 0); }) && ((((tb_o_Node *)tb_nn(v_n, 1433))->q_kids)->len > 0LL))) {
        return 1;
    }
    {
        tb_list * tb_l608 = ((tb_o_Node *)tb_nn(v_n, 1435))->q_kids;
        for (long long tb_i608 = 0; tb_i608 < tb_l608->len; tb_i608++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l608->data)[tb_i608];
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
        tb_list * tb_l609 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_fn, 1441))->q_kids, 0LL, sizeof(tb_o_Node *), 1441)), 1441))->q_kids;
        for (long long tb_i609 = 0; tb_i609 < tb_l609->len; tb_i609++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l609->data)[tb_i609];
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
        long long tb_end610 = ((((tb_o_Node *)tb_nn(v_params, 1447))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end610; v_i++) {
            tb_o_Node * v_p = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1448))->q_kids, v_i, sizeof(tb_o_Node *), 1448));
            {
                long long tb_end611 = (v_i - 1LL);
                for (long long v_j = 0LL; v_j <= tb_end611; v_j++) {
                    if ((strcmp(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1450))->q_kids, v_j, sizeof(tb_o_Node *), 1450)), 1450))->q_name, ((tb_o_Node *)tb_nn(v_p, 1450))->q_name) == 0)) {
                        f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_p, 1451))->q_line, "это имя уже есть в списке", ((tb_o_Node *)tb_nn(v_p, 1451))->q_name);
                    }
                }
            }
            if (((tb_o_Node *)tb_nn(v_p, 1452))->q_has_tsyn) {
                { long long tb_v612 = f_check__resolve_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1453))->q_tsyn); long long *tb_p612 = &(((tb_o_Node *)tb_nn(v_p, 1453))->q_ty); *tb_p612 = tb_v612; }
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
    { long long tb_v613 = (-1LL); *(long long *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1461))->q_temp_types) = tb_v613; }
    { long long tb_v614 = (-1LL); long long *tb_p614 = &(((tb_o_Comp *)tb_nn(v_c, 1462))->q_par_base); *tb_p614 = tb_v614; }
    { long long tb_v615 = (-1LL); long long *tb_p615 = &(((tb_o_Comp *)tb_nn(v_c, 1463))->q_temp_base); *tb_p615 = tb_v615; }
    const char * v_main_mod = "";
    if (((((tb_o_Node *)tb_nn(v_program, 1467))->q_kids)->len > 0LL)) {
        v_main_mod = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1468))->q_kids, 0LL, sizeof(tb_o_Node *), 1468)), 1468))->q_mod;
    }
    {
        tb_list * tb_l616 = ((tb_o_Node *)tb_nn(v_program, 1469))->q_kids;
        for (long long tb_i616 = 0; tb_i616 < tb_l616->len; tb_i616++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l616->data)[tb_i616];
            if ((!f_check__is_module(v_c, ((tb_o_Node *)tb_nn(v_top, 1470))->q_mod))) {
                { const char * tb_v617 = ((tb_o_Node *)tb_nn(v_top, 1471))->q_mod; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1471))->q_all_mods) = tb_v617; }
            }
            if (({ const char *tb_s618 = ((tb_o_Node *)tb_nn(v_top, 1472))->q_kind; ((unsigned char)tb_s618[0] == 117 && (unsigned char)tb_s618[1] == 115 && (unsigned char)tb_s618[2] == 101 && tb_s618[3] == 0); })) {
                if (((((tb_o_Comp *)tb_nn(v_c, 1473))->q_uses)->len == 500LL)) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_top, 1474))->q_line, "слишком много use", "");
                }
                { tb_o_Use * tb_v619 = tb_new_Use(((tb_o_Node *)tb_nn(v_top, 1475))->q_mod, ((tb_o_Node *)tb_nn(v_top, 1475))->q_name); *(tb_o_Use * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1475))->q_uses) = tb_v619; }
            }
        }
    }
    {
        tb_list * tb_l620 = ((tb_o_Node *)tb_nn(v_program, 1477))->q_kids;
        for (long long tb_i620 = 0; tb_i620 < tb_l620->len; tb_i620++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l620->data)[tb_i620];
            { const char * tb_v621 = ((tb_o_Node *)tb_nn(v_top, 1478))->q_file; const char * *tb_p621 = &(((tb_o_Comp *)tb_nn(v_c, 1478))->q_file_name); *tb_p621 = tb_v621; }
            if (({ const char *tb_s622 = ((tb_o_Node *)tb_nn(v_top, 1479))->q_kind; ((unsigned char)tb_s622[0] == 111 && (unsigned char)tb_s622[1] == 98 && (unsigned char)tb_s622[2] == 106 && tb_s622[3] == 0); })) {
                f_check__register_obj(v_c, v_top);
            } else if (({ const char *tb_s623 = ((tb_o_Node *)tb_nn(v_top, 1481))->q_kind; ((unsigned char)tb_s623[0] == 102 && (unsigned char)tb_s623[1] == 117 && (unsigned char)tb_s623[2] == 110 && (unsigned char)tb_s623[3] == 99 && tb_s623[4] == 0); })) {
                f_check__register_func(v_c, v_top);
                { _Bool tb_v624 = f_check__has_fail((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1483))->q_kids, 1LL, sizeof(tb_o_Node *), 1483))); _Bool *tb_p624 = &(((tb_o_Node *)tb_nn(v_top, 1483))->q_can_fail); *tb_p624 = tb_v624; }
                if ((({ const char *tb_s625 = ((tb_o_Node *)tb_nn(v_top, 1484))->q_name; ((unsigned char)tb_s625[0] == 109 && (unsigned char)tb_s625[1] == 97 && (unsigned char)tb_s625[2] == 105 && (unsigned char)tb_s625[3] == 110 && tb_s625[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_top, 1484))->q_mod, v_main_mod) != 0))) {
                    f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(v_top, 1485))->q_line, "main бывает только в главном файле, в модуле её быть не должно", "");
                }
            }
        }
    }
    {
        tb_list * tb_l626 = ((tb_o_Node *)tb_nn(v_program, 1487))->q_kids;
        for (long long tb_i626 = 0; tb_i626 < tb_l626->len; tb_i626++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l626->data)[tb_i626];
            { const char * tb_v627 = ((tb_o_Node *)tb_nn(v_top, 1488))->q_file; const char * *tb_p627 = &(((tb_o_Comp *)tb_nn(v_c, 1488))->q_file_name); *tb_p627 = tb_v627; }
            if (({ const char *tb_s628 = ((tb_o_Node *)tb_nn(v_top, 1489))->q_kind; ((unsigned char)tb_s628[0] == 111 && (unsigned char)tb_s628[1] == 98 && (unsigned char)tb_s628[2] == 106 && tb_s628[3] == 0); })) {
                f_check__resolve_params(v_c, v_top);
            } else if (({ const char *tb_s629 = ((tb_o_Node *)tb_nn(v_top, 1491))->q_kind; ((unsigned char)tb_s629[0] == 102 && (unsigned char)tb_s629[1] == 117 && (unsigned char)tb_s629[2] == 110 && (unsigned char)tb_s629[3] == 99 && tb_s629[4] == 0); })) {
                f_check__resolve_params(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1492))->q_kids, 0LL, sizeof(tb_o_Node *), 1492)));
                f_check__resolve_params(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1493))->q_kids, 2LL, sizeof(tb_o_Node *), 1493)));
                tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1494))->q_kids, 2LL, sizeof(tb_o_Node *), 1494));
                if (((((tb_o_Node *)tb_nn(v_outputs, 1495))->q_kids)->len == 1LL)) {
                    { long long tb_v630 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1496))->q_kids, 0LL, sizeof(tb_o_Node *), 1496)), 1496))->q_ty; long long *tb_p630 = &(((tb_o_Node *)tb_nn(v_top, 1496))->q_ty); *tb_p630 = tb_v630; }
                } else if (((((tb_o_Node *)tb_nn(v_outputs, 1497))->q_kids)->len > 1LL)) {
                    { long long tb_v631 = f_check__multi_type(v_c, v_top); long long *tb_p631 = &(((tb_o_Node *)tb_nn(v_top, 1498))->q_ty); *tb_p631 = tb_v631; }
                } else if ((!f_check__has_value_return((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 1499))->q_kids, 1LL, sizeof(tb_o_Node *), 1499))))) {
                    { long long tb_v632 = 0LL; long long *tb_p632 = &(((tb_o_Node *)tb_nn(v_top, 1501))->q_ty); *tb_p632 = tb_v632; }
                }
            }
        }
    }
    long long v_main_func = f_check__find_func_in(v_c, "main", v_main_mod);
    if (((((tb_o_Node *)tb_nn(v_program, 1503))->q_kids)->len > 0LL)) {
        { const char * tb_v633 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 1504))->q_kids, 0LL, sizeof(tb_o_Node *), 1504)), 1504))->q_file; const char * *tb_p633 = &(((tb_o_Comp *)tb_nn(v_c, 1504))->q_file_name); *tb_p633 = tb_v633; }
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
            long long tb_end634 = ((((tb_o_Comp *)tb_nn(v_c, 1516))->q_funcs)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end634; v_i++) {
                if (((((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1517))->q_funcs, v_i, sizeof(tb_o_Func *), 1517)), 1517))->q_state != 0LL) || (!f_check__links_known(((tb_o_Func *)tb_nn((*(tb_o_Func * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1517))->q_funcs, v_i, sizeof(tb_o_Func *), 1517)), 1517))->q_node)))) {
                    continue;
                }
                f_check__cut_vars(v_c, 0LL);
                { long long tb_v635 = 0LL; long long *tb_p635 = &(((tb_o_Comp *)tb_nn(v_c, 1520))->q_depth); *tb_p635 = tb_v635; }
                { long long tb_v636 = 0LL; long long *tb_p636 = &(((tb_o_Comp *)tb_nn(v_c, 1521))->q_var_base); *tb_p636 = tb_v636; }
                f_check__check_func(v_c, v_i);
                v_progress = 1;
            }
        }
    }
    {
        tb_list * tb_l637 = ((tb_o_Comp *)tb_nn(v_c, 1524))->q_funcs;
        for (long long tb_i637 = 0; tb_i637 < tb_l637->len; tb_i637++) {
            tb_o_Func * const v_f = ((tb_o_Func * *)tb_l637->data)[tb_i637];
            if ((((tb_o_Func *)tb_nn(v_f, 1525))->q_state == 0LL)) {
                { const char * tb_v638 = ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1526))->q_node, 1526))->q_file; const char * *tb_p638 = &(((tb_o_Comp *)tb_nn(v_c, 1526))->q_file_name); *tb_p638 = tb_v638; }
                f_errors__error_at(v_c, ((tb_o_Node *)tb_nn(((tb_o_Func *)tb_nn(v_f, 1527))->q_node, 1527))->q_line, "функция с link ни разу не вызвана, поэтому тип ярлыка неизвестен", ((tb_o_Func *)tb_nn(v_f, 1527))->q_name);
            }
        }
    }
    f_check__check_piece_leaks(v_c);
    f_check__mark_hot_fields(v_c);
}

static void f_emit__put(tb_o_Comp * v_c, const char * v_s) {
    { const char * tb_v639 = v_s; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 10))->q_out) = tb_v639; }
}

static const char * f_emit__fname(tb_o_Node * v_f) {
    long long tb_rl_mod = -1, tb_rc_mod = 0; (void)tb_rl_mod; (void)tb_rc_mod;
    const char * v_mod = ""; tb_rl_mod = -1;
    {
        const char * tb_l640 = ((tb_o_Node *)tb_nn(v_f, 15))->q_mod;
        for (long long tb_i640 = 0; tb_l640[tb_i640] != 0; tb_i640++) {
            const char * const v_ch = tb_chr((unsigned char)tb_l640[tb_i640]);
            long long v_k = tb_code(v_ch, 16);
            if (((((v_k >= 97LL) && (v_k <= 122LL)) || ((v_k >= 65LL) && (v_k <= 90LL))) || ((v_k >= 48LL) && (v_k <= 57LL)))) {
                tb_app(&v_mod, &tb_rl_mod, &tb_rc_mod, v_ch); 
            } else {
                tb_app_n(&v_mod, &tb_rl_mod, &tb_rc_mod, "_", sizeof("_") - 1); 
            }
        }
    }
    return ({ const char *tb_c641_0 = "f_"; long long tb_c641_0n = sizeof("f_") - 1; const char *tb_c641_1 = v_mod; long long tb_c641_1n = (long long)strlen(tb_c641_1); const char *tb_c641_2 = "__"; long long tb_c641_2n = sizeof("__") - 1; const char *tb_c641_3 = ((tb_o_Node *)tb_nn(v_f, 21))->q_name; long long tb_c641_3n = (long long)strlen(tb_c641_3); char *tb_c641r = tb_mem(tb_c641_0n + tb_c641_1n + tb_c641_2n + tb_c641_3n + 1), *tb_c641rw = tb_c641r; memcpy(tb_c641rw, tb_c641_0, tb_c641_0n); tb_c641rw += tb_c641_0n; memcpy(tb_c641rw, tb_c641_1, tb_c641_1n); tb_c641rw += tb_c641_1n; memcpy(tb_c641rw, tb_c641_2, tb_c641_2n); tb_c641rw += tb_c641_2n; memcpy(tb_c641rw, tb_c641_3, tb_c641_3n); tb_c641rw += tb_c641_3n; *tb_c641rw = 0; (const char *)tb_c641r; });
    tb_crash(13, "функция fname дошла до конца без return");
    return "";
}

static const char * f_emit__c_type(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    if (({ const char *tb_s642 = v_k; ((unsigned char)tb_s642[0] == 105 && (unsigned char)tb_s642[1] == 110 && (unsigned char)tb_s642[2] == 116 && tb_s642[3] == 0); })) {
        return "long long";
    }
    if (({ const char *tb_s643 = v_k; ((unsigned char)tb_s643[0] == 100 && (unsigned char)tb_s643[1] == 101 && (unsigned char)tb_s643[2] == 99 && tb_s643[3] == 0); })) {
        return "double";
    }
    if (({ const char *tb_s644 = v_k; ((unsigned char)tb_s644[0] == 98 && (unsigned char)tb_s644[1] == 111 && (unsigned char)tb_s644[2] == 111 && (unsigned char)tb_s644[3] == 108 && tb_s644[4] == 0); })) {
        return "_Bool";
    }
    if (({ const char *tb_s645 = v_k; ((unsigned char)tb_s645[0] == 116 && (unsigned char)tb_s645[1] == 101 && (unsigned char)tb_s645[2] == 120 && (unsigned char)tb_s645[3] == 116 && tb_s645[4] == 0); })) {
        return "const char *";
    }
    if (({ const char *tb_s646 = v_k; ((unsigned char)tb_s646[0] == 108 && (unsigned char)tb_s646[1] == 105 && (unsigned char)tb_s646[2] == 115 && (unsigned char)tb_s646[3] == 116 && tb_s646[4] == 0); })) {
        if (f_check__is_cols_list(v_c, v_t)) {
            return ({ const char *tb_c647_0 = "tb_cols_"; long long tb_c647_0n = sizeof("tb_cols_") - 1; const char *tb_c647_1 = ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 35))->q_types, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 35))->q_types, v_t, sizeof(tb_o_Type *), 35)), 35))->q_elem, sizeof(tb_o_Type *), 35)), 35))->q_obj_node, 35))->q_name; long long tb_c647_1n = (long long)strlen(tb_c647_1); const char *tb_c647_2 = " *"; long long tb_c647_2n = sizeof(" *") - 1; char *tb_c647r = tb_mem(tb_c647_0n + tb_c647_1n + tb_c647_2n + 1), *tb_c647rw = tb_c647r; memcpy(tb_c647rw, tb_c647_0, tb_c647_0n); tb_c647rw += tb_c647_0n; memcpy(tb_c647rw, tb_c647_1, tb_c647_1n); tb_c647rw += tb_c647_1n; memcpy(tb_c647rw, tb_c647_2, tb_c647_2n); tb_c647rw += tb_c647_2n; *tb_c647rw = 0; (const char *)tb_c647r; });
        }
        return "tb_list *";
    }
    if (({ const char *tb_s648 = v_k; ((unsigned char)tb_s648[0] == 100 && (unsigned char)tb_s648[1] == 105 && (unsigned char)tb_s648[2] == 99 && (unsigned char)tb_s648[3] == 116 && tb_s648[4] == 0); })) {
        return "tb_dict *";
    }
    if (({ const char *tb_s649 = v_k; ((unsigned char)tb_s649[0] == 111 && (unsigned char)tb_s649[1] == 98 && (unsigned char)tb_s649[2] == 106 && tb_s649[3] == 0); })) {
        if (f_check__is_cols_obj(v_c, v_t)) {
            return ({ const char *tb_c650_0 = "tb_row_"; long long tb_c650_0n = sizeof("tb_row_") - 1; const char *tb_c650_1 = ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 41))->q_types, v_t, sizeof(tb_o_Type *), 41)), 41))->q_obj_node, 41))->q_name; long long tb_c650_1n = (long long)strlen(tb_c650_1); char *tb_c650r = tb_mem(tb_c650_0n + tb_c650_1n + 1), *tb_c650rw = tb_c650r; memcpy(tb_c650rw, tb_c650_0, tb_c650_0n); tb_c650rw += tb_c650_0n; memcpy(tb_c650rw, tb_c650_1, tb_c650_1n); tb_c650rw += tb_c650_1n; *tb_c650rw = 0; (const char *)tb_c650r; });
        }
        return ({ const char *tb_c651_0 = "tb_o_"; long long tb_c651_0n = sizeof("tb_o_") - 1; const char *tb_c651_1 = ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 42))->q_types, v_t, sizeof(tb_o_Type *), 42)), 42))->q_obj_node, 42))->q_name; long long tb_c651_1n = (long long)strlen(tb_c651_1); const char *tb_c651_2 = " *"; long long tb_c651_2n = sizeof(" *") - 1; char *tb_c651r = tb_mem(tb_c651_0n + tb_c651_1n + tb_c651_2n + 1), *tb_c651rw = tb_c651r; memcpy(tb_c651rw, tb_c651_0, tb_c651_0n); tb_c651rw += tb_c651_0n; memcpy(tb_c651rw, tb_c651_1, tb_c651_1n); tb_c651rw += tb_c651_1n; memcpy(tb_c651rw, tb_c651_2, tb_c651_2n); tb_c651rw += tb_c651_2n; *tb_c651rw = 0; (const char *)tb_c651r; });
    }
    if (({ const char *tb_s652 = v_k; ((unsigned char)tb_s652[0] == 109 && (unsigned char)tb_s652[1] == 117 && (unsigned char)tb_s652[2] == 108 && (unsigned char)tb_s652[3] == 116 && (unsigned char)tb_s652[4] == 105 && tb_s652[5] == 0); })) {
        return ({ const char *tb_c653_0 = "tb_r_"; long long tb_c653_0n = sizeof("tb_r_") - 1; const char *tb_c653_1 = f_emit__fname(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 44))->q_types, v_t, sizeof(tb_o_Type *), 44)), 44))->q_func_node); long long tb_c653_1n = (long long)strlen(tb_c653_1); char *tb_c653r = tb_mem(tb_c653_0n + tb_c653_1n + 1), *tb_c653rw = tb_c653r; memcpy(tb_c653rw, tb_c653_0, tb_c653_0n); tb_c653rw += tb_c653_0n; memcpy(tb_c653rw, tb_c653_1, tb_c653_1n); tb_c653rw += tb_c653_1n; *tb_c653rw = 0; (const char *)tb_c653r; });
    }
    if (({ const char *tb_s654 = v_k; ((unsigned char)tb_s654[0] == 116 && (unsigned char)tb_s654[1] == 97 && (unsigned char)tb_s654[2] == 115 && (unsigned char)tb_s654[3] == 107 && tb_s654[4] == 0); })) {
        return "tb_task *";
    }
    return "void";
    tb_crash(23, "функция c_type дошла до конца без return");
    return "";
}

static void f_emit__indent(tb_o_Comp * v_c, long long v_depth) {
    {
        long long tb_end655 = v_depth;
        for (long long v_i = 1LL; v_i <= tb_end655; v_i++) {
            f_emit__put(v_c, "    ");
        }
    }
}

static _Bool f_emit__is_bits(tb_o_Comp * v_c, long long v_t) {
    return (({ const char *tb_s656 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s656[0] == 108 && (unsigned char)tb_s656[1] == 105 && (unsigned char)tb_s656[2] == 115 && (unsigned char)tb_s656[3] == 116 && tb_s656[4] == 0); }) && (((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 55))->q_types, v_t, sizeof(tb_o_Type *), 55)), 55))->q_elem == 3LL));
    tb_crash(54, "функция is_bits дошла до конца без return");
    return 0;
}

static void f_emit__emit_default(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    if (({ const char *tb_s657 = v_k; ((unsigned char)tb_s657[0] == 105 && (unsigned char)tb_s657[1] == 110 && (unsigned char)tb_s657[2] == 116 && tb_s657[3] == 0); })) {
        f_emit__put(v_c, "0LL");
    } else if (({ const char *tb_s658 = v_k; ((unsigned char)tb_s658[0] == 100 && (unsigned char)tb_s658[1] == 101 && (unsigned char)tb_s658[2] == 99 && tb_s658[3] == 0); })) {
        f_emit__put(v_c, "0.0");
    } else if (({ const char *tb_s659 = v_k; ((unsigned char)tb_s659[0] == 116 && (unsigned char)tb_s659[1] == 101 && (unsigned char)tb_s659[2] == 120 && (unsigned char)tb_s659[3] == 116 && tb_s659[4] == 0); })) {
        f_emit__put(v_c, "\"\"");
    } else if ((({ const char *tb_s660 = v_k; ((unsigned char)tb_s660[0] == 108 && (unsigned char)tb_s660[1] == 105 && (unsigned char)tb_s660[2] == 115 && (unsigned char)tb_s660[3] == 116 && tb_s660[4] == 0); }) && f_check__is_cols_list(v_c, v_t))) {
        f_emit__put(v_c, ({ const char *tb_c661_0 = "tb_cols_new_"; long long tb_c661_0n = sizeof("tb_cols_new_") - 1; const char *tb_c661_1 = f_emit__cols_name(v_c, v_t); long long tb_c661_1n = (long long)strlen(tb_c661_1); const char *tb_c661_2 = "()"; long long tb_c661_2n = sizeof("()") - 1; char *tb_c661r = tb_mem(tb_c661_0n + tb_c661_1n + tb_c661_2n + 1), *tb_c661rw = tb_c661r; memcpy(tb_c661rw, tb_c661_0, tb_c661_0n); tb_c661rw += tb_c661_0n; memcpy(tb_c661rw, tb_c661_1, tb_c661_1n); tb_c661rw += tb_c661_1n; memcpy(tb_c661rw, tb_c661_2, tb_c661_2n); tb_c661rw += tb_c661_2n; *tb_c661rw = 0; (const char *)tb_c661r; }));
    } else if ((({ const char *tb_s662 = v_k; ((unsigned char)tb_s662[0] == 111 && (unsigned char)tb_s662[1] == 98 && (unsigned char)tb_s662[2] == 106 && tb_s662[3] == 0); }) && f_check__is_cols_obj(v_c, v_t))) {
        f_emit__put(v_c, ({ const char *tb_c663_0 = "("; long long tb_c663_0n = sizeof("(") - 1; const char *tb_c663_1 = f_emit__c_type(v_c, v_t); long long tb_c663_1n = (long long)strlen(tb_c663_1); const char *tb_c663_2 = "){0, 0}"; long long tb_c663_2n = sizeof("){0, 0}") - 1; char *tb_c663r = tb_mem(tb_c663_0n + tb_c663_1n + tb_c663_2n + 1), *tb_c663rw = tb_c663r; memcpy(tb_c663rw, tb_c663_0, tb_c663_0n); tb_c663rw += tb_c663_0n; memcpy(tb_c663rw, tb_c663_1, tb_c663_1n); tb_c663rw += tb_c663_1n; memcpy(tb_c663rw, tb_c663_2, tb_c663_2n); tb_c663rw += tb_c663_2n; *tb_c663rw = 0; (const char *)tb_c663r; }));
    } else if ((({ const char *tb_s664 = v_k; ((unsigned char)tb_s664[0] == 108 && (unsigned char)tb_s664[1] == 105 && (unsigned char)tb_s664[2] == 115 && (unsigned char)tb_s664[3] == 116 && tb_s664[4] == 0); }) && f_emit__is_bits(v_c, v_t))) {
        f_emit__put(v_c, "tb_bits_new()");
    } else if (({ const char *tb_s665 = v_k; ((unsigned char)tb_s665[0] == 108 && (unsigned char)tb_s665[1] == 105 && (unsigned char)tb_s665[2] == 115 && (unsigned char)tb_s665[3] == 116 && tb_s665[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c666_0 = "tb_list_new(sizeof("; long long tb_c666_0n = sizeof("tb_list_new(sizeof(") - 1; const char *tb_c666_1 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 72))->q_types, v_t, sizeof(tb_o_Type *), 72)), 72))->q_elem); long long tb_c666_1n = (long long)strlen(tb_c666_1); const char *tb_c666_2 = "))"; long long tb_c666_2n = sizeof("))") - 1; char *tb_c666r = tb_mem(tb_c666_0n + tb_c666_1n + tb_c666_2n + 1), *tb_c666rw = tb_c666r; memcpy(tb_c666rw, tb_c666_0, tb_c666_0n); tb_c666rw += tb_c666_0n; memcpy(tb_c666rw, tb_c666_1, tb_c666_1n); tb_c666rw += tb_c666_1n; memcpy(tb_c666rw, tb_c666_2, tb_c666_2n); tb_c666rw += tb_c666_2n; *tb_c666rw = 0; (const char *)tb_c666r; }));
    } else if (({ const char *tb_s667 = v_k; ((unsigned char)tb_s667[0] == 100 && (unsigned char)tb_s667[1] == 105 && (unsigned char)tb_s667[2] == 99 && (unsigned char)tb_s667[3] == 116 && tb_s667[4] == 0); })) {
        const char * v_is_text = "0";
        if (({ const char *tb_s668 = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 75))->q_types, v_t, sizeof(tb_o_Type *), 75)), 75))->q_key); ((unsigned char)tb_s668[0] == 116 && (unsigned char)tb_s668[1] == 101 && (unsigned char)tb_s668[2] == 120 && (unsigned char)tb_s668[3] == 116 && tb_s668[4] == 0); })) {
            v_is_text = "1";
        }
        f_emit__put(v_c, ({ const char *tb_c669_0 = "tb_dict_new("; long long tb_c669_0n = sizeof("tb_dict_new(") - 1; const char *tb_c669_1 = v_is_text; long long tb_c669_1n = (long long)strlen(tb_c669_1); const char *tb_c669_2 = ", sizeof("; long long tb_c669_2n = sizeof(", sizeof(") - 1; const char *tb_c669_3 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 77))->q_types, v_t, sizeof(tb_o_Type *), 77)), 77))->q_elem); long long tb_c669_3n = (long long)strlen(tb_c669_3); const char *tb_c669_4 = "))"; long long tb_c669_4n = sizeof("))") - 1; char *tb_c669r = tb_mem(tb_c669_0n + tb_c669_1n + tb_c669_2n + tb_c669_3n + tb_c669_4n + 1), *tb_c669rw = tb_c669r; memcpy(tb_c669rw, tb_c669_0, tb_c669_0n); tb_c669rw += tb_c669_0n; memcpy(tb_c669rw, tb_c669_1, tb_c669_1n); tb_c669rw += tb_c669_1n; memcpy(tb_c669rw, tb_c669_2, tb_c669_2n); tb_c669rw += tb_c669_2n; memcpy(tb_c669rw, tb_c669_3, tb_c669_3n); tb_c669rw += tb_c669_3n; memcpy(tb_c669rw, tb_c669_4, tb_c669_4n); tb_c669rw += tb_c669_4n; *tb_c669rw = 0; (const char *)tb_c669r; }));
    } else {
        f_emit__put(v_c, "0");
    }
}

static const char * f_emit__cols_name(tb_o_Comp * v_c, long long v_t) {
    if (({ const char *tb_s670 = f_check__kind_of(v_c, v_t); ((unsigned char)tb_s670[0] == 108 && (unsigned char)tb_s670[1] == 105 && (unsigned char)tb_s670[2] == 115 && (unsigned char)tb_s670[3] == 116 && tb_s670[4] == 0); })) {
        return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 84))->q_types, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 84))->q_types, v_t, sizeof(tb_o_Type *), 84)), 84))->q_elem, sizeof(tb_o_Type *), 84)), 84))->q_obj_node, 84))->q_name;
    }
    return ((tb_o_Node *)tb_nn(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 85))->q_types, v_t, sizeof(tb_o_Type *), 85)), 85))->q_obj_node, 85))->q_name;
    tb_crash(82, "функция cols_name дошла до конца без return");
    return "";
}

static _Bool f_emit__is_simple(tb_o_Comp * v_c, long long v_t) {
    const char * v_k = f_check__kind_of(v_c, v_t);
    return (((({ const char *tb_s671 = v_k; ((unsigned char)tb_s671[0] == 105 && (unsigned char)tb_s671[1] == 110 && (unsigned char)tb_s671[2] == 116 && tb_s671[3] == 0); }) || ({ const char *tb_s672 = v_k; ((unsigned char)tb_s672[0] == 100 && (unsigned char)tb_s672[1] == 101 && (unsigned char)tb_s672[2] == 99 && tb_s672[3] == 0); })) || ({ const char *tb_s673 = v_k; ((unsigned char)tb_s673[0] == 98 && (unsigned char)tb_s673[1] == 111 && (unsigned char)tb_s673[2] == 111 && (unsigned char)tb_s673[3] == 108 && tb_s673[4] == 0); })) || ({ const char *tb_s674 = v_k; ((unsigned char)tb_s674[0] == 116 && (unsigned char)tb_s674[1] == 101 && (unsigned char)tb_s674[2] == 120 && (unsigned char)tb_s674[3] == 116 && tb_s674[4] == 0); }));
    tb_crash(87, "функция is_simple дошла до конца без return");
    return 0;
}

static void f_emit__emit_key(tb_o_Comp * v_c, long long v_dt, tb_o_Node * v_key) {
    f_emit__put(v_c, ({ const char *tb_c675_0 = "&("; long long tb_c675_0n = sizeof("&(") - 1; const char *tb_c675_1 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 93))->q_types, v_dt, sizeof(tb_o_Type *), 93)), 93))->q_key); long long tb_c675_1n = (long long)strlen(tb_c675_1); const char *tb_c675_2 = "){"; long long tb_c675_2n = sizeof("){") - 1; char *tb_c675r = tb_mem(tb_c675_0n + tb_c675_1n + tb_c675_2n + 1), *tb_c675rw = tb_c675r; memcpy(tb_c675rw, tb_c675_0, tb_c675_0n); tb_c675rw += tb_c675_0n; memcpy(tb_c675rw, tb_c675_1, tb_c675_1n); tb_c675rw += tb_c675_1n; memcpy(tb_c675rw, tb_c675_2, tb_c675_2n); tb_c675rw += tb_c675_2n; *tb_c675rw = 0; (const char *)tb_c675r; }));
    f_emit__emit_expr(v_c, v_key);
    f_emit__put(v_c, "}");
}

static void f_emit__emit_as(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_want) {
    if ((({ const char *tb_s676 = f_check__kind_of(v_c, v_want); ((unsigned char)tb_s676[0] == 100 && (unsigned char)tb_s676[1] == 101 && (unsigned char)tb_s676[2] == 99 && tb_s676[3] == 0); }) && ({ const char *tb_s677 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 101))->q_ty); ((unsigned char)tb_s677[0] == 105 && (unsigned char)tb_s677[1] == 110 && (unsigned char)tb_s677[2] == 116 && tb_s677[3] == 0); }))) {
        f_emit__put(v_c, "(double)(");
        f_emit__emit_expr(v_c, v_n);
        f_emit__put(v_c, ")");
        return;
    }
    f_emit__emit_expr(v_c, v_n);
}

static void f_emit__emit_as_text(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 110))->q_ty);
    if (({ const char *tb_s678 = v_k; ((unsigned char)tb_s678[0] == 116 && (unsigned char)tb_s678[1] == 101 && (unsigned char)tb_s678[2] == 120 && (unsigned char)tb_s678[3] == 116 && tb_s678[4] == 0); })) {
        f_emit__emit_q(v_c, v_n);
        return;
    }
    if (({ const char *tb_s679 = v_k; ((unsigned char)tb_s679[0] == 105 && (unsigned char)tb_s679[1] == 110 && (unsigned char)tb_s679[2] == 116 && tb_s679[3] == 0); })) {
        f_emit__put(v_c, "tb_itos(");
    } else if (({ const char *tb_s680 = v_k; ((unsigned char)tb_s680[0] == 100 && (unsigned char)tb_s680[1] == 101 && (unsigned char)tb_s680[2] == 99 && tb_s680[3] == 0); })) {
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
        long long tb_end681 = ((((tb_o_Node *)tb_nn(v_n, 125))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end681; v_i++) {
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 126))->q_kids, v_i, sizeof(tb_o_Node *), 126)), 126))->q_kids, 0LL, sizeof(tb_o_Node *), 126));
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", tb_ps(\" \"), ");
            }
            f_emit__put(v_c, ({ const char *tb_c682_0 = "tb_show_"; long long tb_c682_0n = sizeof("tb_show_") - 1; char tb_c682_1d[24]; long long tb_c682_1n; const char *tb_c682_1 = tb_digits(((tb_o_Node *)tb_nn(v_value, 129))->q_ty, tb_c682_1d, &tb_c682_1n); const char *tb_c682_2 = "("; long long tb_c682_2n = sizeof("(") - 1; char *tb_c682r = tb_mem(tb_c682_0n + tb_c682_1n + tb_c682_2n + 1), *tb_c682rw = tb_c682r; memcpy(tb_c682rw, tb_c682_0, tb_c682_0n); tb_c682rw += tb_c682_0n; memcpy(tb_c682rw, tb_c682_1, tb_c682_1n); tb_c682rw += tb_c682_1n; memcpy(tb_c682rw, tb_c682_2, tb_c682_2n); tb_c682rw += tb_c682_2n; *tb_c682rw = 0; (const char *)tb_c682r; }));
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
        long long tb_end683 = ((((tb_o_Node *)tb_nn(v_n, 138))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end683; v_i++) {
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
        f_emit__put(v_c, ({ const char *tb_c684_0 = ""; long long tb_c684_0n = sizeof("") - 1; char tb_c684_1d[24]; long long tb_c684_1n; const char *tb_c684_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 145))->q_line, tb_c684_1d, &tb_c684_1n); char *tb_c684r = tb_mem(tb_c684_0n + tb_c684_1n + 1), *tb_c684rw = tb_c684r; memcpy(tb_c684rw, tb_c684_0, tb_c684_0n); tb_c684rw += tb_c684_0n; memcpy(tb_c684rw, tb_c684_1, tb_c684_1n); tb_c684rw += tb_c684_1n; *tb_c684rw = 0; (const char *)tb_c684r; }));
    }
    f_emit__put(v_c, ")");
}

static void f_emit__emit_builtin(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_name = ((tb_o_Node *)tb_nn(v_n, 149))->q_name;
    if (({ const char *tb_s685 = v_name; ((unsigned char)tb_s685[0] == 112 && (unsigned char)tb_s685[1] == 114 && (unsigned char)tb_s685[2] == 105 && (unsigned char)tb_s685[3] == 110 && (unsigned char)tb_s685[4] == 116 && tb_s685[5] == 0); })) {
        f_emit__emit_print(v_c, v_n);
        return;
    }
    if (({ const char *tb_s686 = v_name; ((unsigned char)tb_s686[0] == 97 && (unsigned char)tb_s686[1] == 114 && (unsigned char)tb_s686[2] == 103 && (unsigned char)tb_s686[3] == 115 && tb_s686[4] == 0); })) {
        f_emit__put(v_c, "tb_args");
        return;
    }
    if (({ const char *tb_s687 = v_name; ((unsigned char)tb_s687[0] == 105 && (unsigned char)tb_s687[1] == 110 && (unsigned char)tb_s687[2] == 112 && (unsigned char)tb_s687[3] == 117 && (unsigned char)tb_s687[4] == 116 && tb_s687[5] == 0); })) {
        f_emit__put(v_c, "tb_input()");
        return;
    }
    tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 159))->q_kids, 0LL, sizeof(tb_o_Node *), 159)), 159))->q_kids, 0LL, sizeof(tb_o_Node *), 159));
    if (({ const char *tb_s688 = v_name; ((unsigned char)tb_s688[0] == 108 && (unsigned char)tb_s688[1] == 101 && (unsigned char)tb_s688[2] == 110 && tb_s688[3] == 0); })) {
        const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 161))->q_ty);
        if ((({ const char *tb_s689 = v_k; ((unsigned char)tb_s689[0] == 116 && (unsigned char)tb_s689[1] == 101 && (unsigned char)tb_s689[2] == 120 && (unsigned char)tb_s689[3] == 116 && tb_s689[4] == 0); }) && f_emit__is_bld(v_c, v_a))) {
            f_emit__put(v_c, ({ const char *tb_c690_0 = "(tb_rl_"; long long tb_c690_0n = sizeof("(tb_rl_") - 1; const char *tb_c690_1 = ((tb_o_Node *)tb_nn(v_a, 163))->q_name; long long tb_c690_1n = (long long)strlen(tb_c690_1); const char *tb_c690_2 = " >= 0 ? tb_rl_"; long long tb_c690_2n = sizeof(" >= 0 ? tb_rl_") - 1; const char *tb_c690_3 = ((tb_o_Node *)tb_nn(v_a, 163))->q_name; long long tb_c690_3n = (long long)strlen(tb_c690_3); const char *tb_c690_4 = " : tb_tlen(v_"; long long tb_c690_4n = sizeof(" : tb_tlen(v_") - 1; const char *tb_c690_5 = ((tb_o_Node *)tb_nn(v_a, 163))->q_name; long long tb_c690_5n = (long long)strlen(tb_c690_5); const char *tb_c690_6 = "))"; long long tb_c690_6n = sizeof("))") - 1; char *tb_c690r = tb_mem(tb_c690_0n + tb_c690_1n + tb_c690_2n + tb_c690_3n + tb_c690_4n + tb_c690_5n + tb_c690_6n + 1), *tb_c690rw = tb_c690r; memcpy(tb_c690rw, tb_c690_0, tb_c690_0n); tb_c690rw += tb_c690_0n; memcpy(tb_c690rw, tb_c690_1, tb_c690_1n); tb_c690rw += tb_c690_1n; memcpy(tb_c690rw, tb_c690_2, tb_c690_2n); tb_c690rw += tb_c690_2n; memcpy(tb_c690rw, tb_c690_3, tb_c690_3n); tb_c690rw += tb_c690_3n; memcpy(tb_c690rw, tb_c690_4, tb_c690_4n); tb_c690rw += tb_c690_4n; memcpy(tb_c690rw, tb_c690_5, tb_c690_5n); tb_c690rw += tb_c690_5n; memcpy(tb_c690rw, tb_c690_6, tb_c690_6n); tb_c690rw += tb_c690_6n; *tb_c690rw = 0; (const char *)tb_c690r; }));
        } else if (({ const char *tb_s691 = v_k; ((unsigned char)tb_s691[0] == 116 && (unsigned char)tb_s691[1] == 101 && (unsigned char)tb_s691[2] == 120 && (unsigned char)tb_s691[3] == 116 && tb_s691[4] == 0); })) {
            f_emit__put(v_c, "tb_tlen(");
            f_emit__emit_expr(v_c, v_a);
            f_emit__put(v_c, ")");
        } else if (({ const char *tb_s692 = v_k; ((unsigned char)tb_s692[0] == 100 && (unsigned char)tb_s692[1] == 105 && (unsigned char)tb_s692[2] == 99 && (unsigned char)tb_s692[3] == 116 && tb_s692[4] == 0); })) {
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
    if (({ const char *tb_s693 = v_name; ((unsigned char)tb_s693[0] == 115 && (unsigned char)tb_s693[1] == 111 && (unsigned char)tb_s693[2] == 114 && (unsigned char)tb_s693[3] == 116 && tb_s693[4] == 0); })) {
        const char * v_k = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 178))->q_types, ((tb_o_Node *)tb_nn(v_a, 178))->q_ty, sizeof(tb_o_Type *), 178)), 178))->q_elem);
        f_emit__put(v_c, "tb_sort_list(");
        f_emit__emit_expr(v_c, v_a);
        if (({ const char *tb_s694 = v_k; ((unsigned char)tb_s694[0] == 105 && (unsigned char)tb_s694[1] == 110 && (unsigned char)tb_s694[2] == 116 && tb_s694[3] == 0); })) {
            f_emit__put(v_c, ", tb_cmp_int)");
        } else if (({ const char *tb_s695 = v_k; ((unsigned char)tb_s695[0] == 100 && (unsigned char)tb_s695[1] == 101 && (unsigned char)tb_s695[2] == 99 && tb_s695[3] == 0); })) {
            f_emit__put(v_c, ", tb_cmp_dec)");
        } else {
            f_emit__put(v_c, ", tb_cmp_text)");
        }
        return;
    }
    if (({ const char *tb_s696 = v_name; ((unsigned char)tb_s696[0] == 97 && (unsigned char)tb_s696[1] == 98 && (unsigned char)tb_s696[2] == 115 && tb_s696[3] == 0); })) {
        if (({ const char *tb_s697 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 189))->q_ty); ((unsigned char)tb_s697[0] == 105 && (unsigned char)tb_s697[1] == 110 && (unsigned char)tb_s697[2] == 116 && tb_s697[3] == 0); })) {
            f_emit__put(v_c, "tb_absi(");
        } else {
            f_emit__put(v_c, "fabs(");
        }
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ")");
        return;
    }
    if (({ const char *tb_s698 = v_name; ((unsigned char)tb_s698[0] == 115 && (unsigned char)tb_s698[1] == 113 && (unsigned char)tb_s698[2] == 114 && (unsigned char)tb_s698[3] == 116 && tb_s698[4] == 0); })) {
        f_emit__put(v_c, "tb_sqrt((double)(");
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ({ const char *tb_c699_0 = "), "; long long tb_c699_0n = sizeof("), ") - 1; char tb_c699_1d[24]; long long tb_c699_1n; const char *tb_c699_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 199))->q_line, tb_c699_1d, &tb_c699_1n); const char *tb_c699_2 = ")"; long long tb_c699_2n = sizeof(")") - 1; char *tb_c699r = tb_mem(tb_c699_0n + tb_c699_1n + tb_c699_2n + 1), *tb_c699rw = tb_c699r; memcpy(tb_c699rw, tb_c699_0, tb_c699_0n); tb_c699rw += tb_c699_0n; memcpy(tb_c699rw, tb_c699_1, tb_c699_1n); tb_c699rw += tb_c699_1n; memcpy(tb_c699rw, tb_c699_2, tb_c699_2n); tb_c699rw += tb_c699_2n; *tb_c699rw = 0; (const char *)tb_c699r; }));
        return;
    }
    if ((({ const char *tb_s700 = v_name; ((unsigned char)tb_s700[0] == 109 && (unsigned char)tb_s700[1] == 105 && (unsigned char)tb_s700[2] == 110 && tb_s700[3] == 0); }) || ({ const char *tb_s701 = v_name; ((unsigned char)tb_s701[0] == 109 && (unsigned char)tb_s701[1] == 97 && (unsigned char)tb_s701[2] == 120 && tb_s701[3] == 0); }))) {
        _Bool v_ints = ({ const char *tb_s702 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 202))->q_ty); ((unsigned char)tb_s702[0] == 105 && (unsigned char)tb_s702[1] == 110 && (unsigned char)tb_s702[2] == 116 && tb_s702[3] == 0); });
        if (v_ints) {
            f_emit__put(v_c, ({ const char *tb_c703_0 = "tb_"; long long tb_c703_0n = sizeof("tb_") - 1; const char *tb_c703_1 = v_name; long long tb_c703_1n = (long long)strlen(tb_c703_1); const char *tb_c703_2 = "i("; long long tb_c703_2n = sizeof("i(") - 1; char *tb_c703r = tb_mem(tb_c703_0n + tb_c703_1n + tb_c703_2n + 1), *tb_c703rw = tb_c703r; memcpy(tb_c703rw, tb_c703_0, tb_c703_0n); tb_c703rw += tb_c703_0n; memcpy(tb_c703rw, tb_c703_1, tb_c703_1n); tb_c703rw += tb_c703_1n; memcpy(tb_c703rw, tb_c703_2, tb_c703_2n); tb_c703rw += tb_c703_2n; *tb_c703rw = 0; (const char *)tb_c703r; }));
            f_emit__emit_expr(v_c, v_a);
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 207))->q_kids, 1LL, sizeof(tb_o_Node *), 207)), 207))->q_kids, 0LL, sizeof(tb_o_Node *), 207)));
        } else {
            f_emit__put(v_c, ({ const char *tb_c704_0 = "tb_"; long long tb_c704_0n = sizeof("tb_") - 1; const char *tb_c704_1 = v_name; long long tb_c704_1n = (long long)strlen(tb_c704_1); const char *tb_c704_2 = "d("; long long tb_c704_2n = sizeof("d(") - 1; char *tb_c704r = tb_mem(tb_c704_0n + tb_c704_1n + tb_c704_2n + 1), *tb_c704rw = tb_c704r; memcpy(tb_c704rw, tb_c704_0, tb_c704_0n); tb_c704rw += tb_c704_0n; memcpy(tb_c704rw, tb_c704_1, tb_c704_1n); tb_c704rw += tb_c704_1n; memcpy(tb_c704rw, tb_c704_2, tb_c704_2n); tb_c704rw += tb_c704_2n; *tb_c704rw = 0; (const char *)tb_c704r; }));
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
            f_emit__put(v_c, ({ const char *tb_c705_0 = (*(const char * *)tb_at(v_c_names, (v_i + 1LL), sizeof(const char *), 220)); long long tb_c705_0n = (long long)strlen(tb_c705_0); const char *tb_c705_1 = "("; long long tb_c705_1n = sizeof("(") - 1; char *tb_c705r = tb_mem(tb_c705_0n + tb_c705_1n + 1), *tb_c705rw = tb_c705r; memcpy(tb_c705rw, tb_c705_0, tb_c705_0n); tb_c705rw += tb_c705_0n; memcpy(tb_c705rw, tb_c705_1, tb_c705_1n); tb_c705rw += tb_c705_1n; *tb_c705rw = 0; (const char *)tb_c705r; }));
            f_emit__emit_args(v_c, v_n, ({ const char *tb_s706 = (*(const char * *)tb_at(v_c_names, (v_i + 2LL), sizeof(const char *), 221)); ((unsigned char)tb_s706[0] == 49 && tb_s706[1] == 0); }));
            return;
        }
        { long long tb_v707 = 3LL; long long *tb_p707 = &(v_i); *tb_p707 += tb_v707; }
    }
    f_emit__put(v_c, ({ const char *tb_c708_0 = "/* неизвестная встроенная "; long long tb_c708_0n = sizeof("/* неизвестная встроенная ") - 1; const char *tb_c708_1 = v_name; long long tb_c708_1n = (long long)strlen(tb_c708_1); const char *tb_c708_2 = " */0"; long long tb_c708_2n = sizeof(" */0") - 1; char *tb_c708r = tb_mem(tb_c708_0n + tb_c708_1n + tb_c708_2n + 1), *tb_c708rw = tb_c708r; memcpy(tb_c708rw, tb_c708_0, tb_c708_0n); tb_c708rw += tb_c708_0n; memcpy(tb_c708rw, tb_c708_1, tb_c708_1n); tb_c708rw += tb_c708_1n; memcpy(tb_c708rw, tb_c708_2, tb_c708_2n); tb_c708rw += tb_c708_2n; *tb_c708rw = 0; (const char *)tb_c708r; }));
}

static void f_emit__emit_call(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if ((!((tb_o_Node *)tb_nn(v_n, 227))->q_has_ref)) {
        f_emit__emit_builtin(v_c, v_n);
        return;
    }
    if (({ const char *tb_s709 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 230))->q_ref, 230))->q_kind; ((unsigned char)tb_s709[0] == 111 && (unsigned char)tb_s709[1] == 98 && (unsigned char)tb_s709[2] == 106 && tb_s709[3] == 0); })) {
        tb_o_Node * v_o = ((tb_o_Node *)tb_nn(v_n, 231))->q_ref;
        f_emit__put(v_c, ({ const char *tb_c710_0 = "tb_new_"; long long tb_c710_0n = sizeof("tb_new_") - 1; const char *tb_c710_1 = ((tb_o_Node *)tb_nn(v_o, 232))->q_name; long long tb_c710_1n = (long long)strlen(tb_c710_1); const char *tb_c710_2 = "("; long long tb_c710_2n = sizeof("(") - 1; char *tb_c710r = tb_mem(tb_c710_0n + tb_c710_1n + tb_c710_2n + 1), *tb_c710rw = tb_c710r; memcpy(tb_c710rw, tb_c710_0, tb_c710_0n); tb_c710rw += tb_c710_0n; memcpy(tb_c710rw, tb_c710_1, tb_c710_1n); tb_c710rw += tb_c710_1n; memcpy(tb_c710rw, tb_c710_2, tb_c710_2n); tb_c710rw += tb_c710_2n; *tb_c710rw = 0; (const char *)tb_c710r; }));
        {
            long long tb_end711 = ((((tb_o_Node *)tb_nn(v_o, 233))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end711; v_i++) {
                if ((v_i > 0LL)) {
                    f_emit__put(v_c, ", ");
                }
                if (({ const char *tb_s712 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 236))->q_kids, v_i, sizeof(tb_o_Node *), 236)), 236))->q_kind; ((unsigned char)tb_s712[0] == 109 && (unsigned char)tb_s712[1] == 105 && (unsigned char)tb_s712[2] == 115 && (unsigned char)tb_s712[3] == 115 && (unsigned char)tb_s712[4] == 105 && (unsigned char)tb_s712[5] == 110 && (unsigned char)tb_s712[6] == 103 && tb_s712[7] == 0); })) {
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
    f_emit__put(v_c, ({ const char *tb_c713_0 = f_emit__fname(((tb_o_Node *)tb_nn(v_n, 243))->q_ref); long long tb_c713_0n = (long long)strlen(tb_c713_0); const char *tb_c713_1 = "("; long long tb_c713_1n = sizeof("(") - 1; char *tb_c713r = tb_mem(tb_c713_0n + tb_c713_1n + 1), *tb_c713rw = tb_c713r; memcpy(tb_c713rw, tb_c713_0, tb_c713_0n); tb_c713rw += tb_c713_0n; memcpy(tb_c713rw, tb_c713_1, tb_c713_1n); tb_c713rw += tb_c713_1n; *tb_c713rw = 0; (const char *)tb_c713r; }));
    {
        long long tb_end714 = ((((tb_o_Node *)tb_nn(v_n, 244))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end714; v_i++) {
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
    if (({ const char *tb_s715 = f_check__kind_of(v_c, ((tb_o_Type *)tb_nn(v_t, 260))->q_key); ((unsigned char)tb_s715[0] == 116 && (unsigned char)tb_s715[1] == 101 && (unsigned char)tb_s715[2] == 120 && (unsigned char)tb_s715[3] == 116 && tb_s715[4] == 0); })) {
        v_is_text = "1";
    }
    f_emit__put(v_c, ({ const char *tb_c716_0 = "tb_dict_from("; long long tb_c716_0n = sizeof("tb_dict_from(") - 1; const char *tb_c716_1 = v_is_text; long long tb_c716_1n = (long long)strlen(tb_c716_1); const char *tb_c716_2 = ", sizeof("; long long tb_c716_2n = sizeof(", sizeof(") - 1; const char *tb_c716_3 = v_vt; long long tb_c716_3n = (long long)strlen(tb_c716_3); const char *tb_c716_4 = "), "; long long tb_c716_4n = sizeof("), ") - 1; char tb_c716_5d[24]; long long tb_c716_5n; const char *tb_c716_5 = tb_digits(tb_div((((tb_o_Node *)tb_nn(v_n, 262))->q_kids)->len, 2LL, 262), tb_c716_5d, &tb_c716_5n); const char *tb_c716_6 = ", ("; long long tb_c716_6n = sizeof(", (") - 1; const char *tb_c716_7 = v_kt; long long tb_c716_7n = (long long)strlen(tb_c716_7); const char *tb_c716_8 = "[]){"; long long tb_c716_8n = sizeof("[]){") - 1; char *tb_c716r = tb_mem(tb_c716_0n + tb_c716_1n + tb_c716_2n + tb_c716_3n + tb_c716_4n + tb_c716_5n + tb_c716_6n + tb_c716_7n + tb_c716_8n + 1), *tb_c716rw = tb_c716r; memcpy(tb_c716rw, tb_c716_0, tb_c716_0n); tb_c716rw += tb_c716_0n; memcpy(tb_c716rw, tb_c716_1, tb_c716_1n); tb_c716rw += tb_c716_1n; memcpy(tb_c716rw, tb_c716_2, tb_c716_2n); tb_c716rw += tb_c716_2n; memcpy(tb_c716rw, tb_c716_3, tb_c716_3n); tb_c716rw += tb_c716_3n; memcpy(tb_c716rw, tb_c716_4, tb_c716_4n); tb_c716rw += tb_c716_4n; memcpy(tb_c716rw, tb_c716_5, tb_c716_5n); tb_c716rw += tb_c716_5n; memcpy(tb_c716rw, tb_c716_6, tb_c716_6n); tb_c716rw += tb_c716_6n; memcpy(tb_c716rw, tb_c716_7, tb_c716_7n); tb_c716rw += tb_c716_7n; memcpy(tb_c716rw, tb_c716_8, tb_c716_8n); tb_c716rw += tb_c716_8n; *tb_c716rw = 0; (const char *)tb_c716r; }));
    long long v_i = 0LL;
    while ((v_i < (((tb_o_Node *)tb_nn(v_n, 264))->q_kids)->len)) {
        if ((v_i > 0LL)) {
            f_emit__put(v_c, ", ");
        }
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 267))->q_kids, v_i, sizeof(tb_o_Node *), 267)));
        { long long tb_v717 = 2LL; long long *tb_p717 = &(v_i); *tb_p717 += tb_v717; }
    }
    f_emit__put(v_c, ({ const char *tb_c718_0 = "}, ("; long long tb_c718_0n = sizeof("}, (") - 1; const char *tb_c718_1 = v_vt; long long tb_c718_1n = (long long)strlen(tb_c718_1); const char *tb_c718_2 = "[]){"; long long tb_c718_2n = sizeof("[]){") - 1; char *tb_c718r = tb_mem(tb_c718_0n + tb_c718_1n + tb_c718_2n + 1), *tb_c718rw = tb_c718r; memcpy(tb_c718rw, tb_c718_0, tb_c718_0n); tb_c718rw += tb_c718_0n; memcpy(tb_c718rw, tb_c718_1, tb_c718_1n); tb_c718rw += tb_c718_1n; memcpy(tb_c718rw, tb_c718_2, tb_c718_2n); tb_c718rw += tb_c718_2n; *tb_c718rw = 0; (const char *)tb_c718r; }));
    v_i = 1LL;
    while ((v_i < (((tb_o_Node *)tb_nn(v_n, 271))->q_kids)->len)) {
        if ((v_i > 1LL)) {
            f_emit__put(v_c, ", ");
        }
        f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 274))->q_kids, v_i, sizeof(tb_o_Node *), 274)), ((tb_o_Type *)tb_nn(v_t, 274))->q_elem);
        { long long tb_v719 = 2LL; long long *tb_p719 = &(v_i); *tb_p719 += tb_v719; }
    }
    f_emit__put(v_c, "})");
}

static void f_emit__emit_method(tb_o_Comp * v_c, tb_o_Node * v_n) {
    tb_o_Node * v_base = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 280))->q_kids, 0LL, sizeof(tb_o_Node *), 280));
    if (({ const char *tb_s720 = ((tb_o_Node *)tb_nn(v_n, 281))->q_name; ((unsigned char)tb_s720[0] == 99 && (unsigned char)tb_s720[1] == 108 && (unsigned char)tb_s720[2] == 101 && (unsigned char)tb_s720[3] == 97 && (unsigned char)tb_s720[4] == 114 && tb_s720[5] == 0); })) {
        if (({ const char *tb_s721 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_base, 282))->q_ty); ((unsigned char)tb_s721[0] == 100 && (unsigned char)tb_s721[1] == 105 && (unsigned char)tb_s721[2] == 99 && (unsigned char)tb_s721[3] == 116 && tb_s721[4] == 0); })) {
            f_emit__put(v_c, "tb_dclear(");
        } else {
            f_emit__put(v_c, "tb_lclear(");
        }
        f_emit__emit_expr(v_c, v_base);
        f_emit__put(v_c, ({ const char *tb_c722_0 = ", "; long long tb_c722_0n = sizeof(", ") - 1; char tb_c722_1d[24]; long long tb_c722_1n; const char *tb_c722_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 287))->q_line, tb_c722_1d, &tb_c722_1n); const char *tb_c722_2 = ")"; long long tb_c722_2n = sizeof(")") - 1; char *tb_c722r = tb_mem(tb_c722_0n + tb_c722_1n + tb_c722_2n + 1), *tb_c722rw = tb_c722r; memcpy(tb_c722rw, tb_c722_0, tb_c722_0n); tb_c722rw += tb_c722_0n; memcpy(tb_c722rw, tb_c722_1, tb_c722_1n); tb_c722rw += tb_c722_1n; memcpy(tb_c722rw, tb_c722_2, tb_c722_2n); tb_c722rw += tb_c722_2n; *tb_c722rw = 0; (const char *)tb_c722r; }));
        return;
    }
    if (({ const char *tb_s723 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_base, 289))->q_ty); ((unsigned char)tb_s723[0] == 100 && (unsigned char)tb_s723[1] == 105 && (unsigned char)tb_s723[2] == 99 && (unsigned char)tb_s723[3] == 116 && tb_s723[4] == 0); })) {
        if ((({ const char *tb_s724 = ((tb_o_Node *)tb_nn(v_n, 290))->q_name; ((unsigned char)tb_s724[0] == 118 && (unsigned char)tb_s724[1] == 97 && (unsigned char)tb_s724[2] == 108 && (unsigned char)tb_s724[3] == 117 && (unsigned char)tb_s724[4] == 101 && (unsigned char)tb_s724[5] == 115 && tb_s724[6] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_n, 290))->q_ty))) {
            f_emit__put(v_c, ({ const char *tb_c725_0 = "tb_colsfrom_"; long long tb_c725_0n = sizeof("tb_colsfrom_") - 1; const char *tb_c725_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn(v_n, 291))->q_ty); long long tb_c725_1n = (long long)strlen(tb_c725_1); const char *tb_c725_2 = "(("; long long tb_c725_2n = sizeof("((") - 1; char *tb_c725r = tb_mem(tb_c725_0n + tb_c725_1n + tb_c725_2n + 1), *tb_c725rw = tb_c725r; memcpy(tb_c725rw, tb_c725_0, tb_c725_0n); tb_c725rw += tb_c725_0n; memcpy(tb_c725rw, tb_c725_1, tb_c725_1n); tb_c725rw += tb_c725_1n; memcpy(tb_c725rw, tb_c725_2, tb_c725_2n); tb_c725rw += tb_c725_2n; *tb_c725rw = 0; (const char *)tb_c725r; }));
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, ({ const char *tb_c726_0 = ")->vals->len, ("; long long tb_c726_0n = sizeof(")->vals->len, (") - 1; const char *tb_c726_1 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 293))->q_types, ((tb_o_Node *)tb_nn(v_n, 293))->q_ty, sizeof(tb_o_Type *), 293)), 293))->q_elem); long long tb_c726_1n = (long long)strlen(tb_c726_1); const char *tb_c726_2 = " *)("; long long tb_c726_2n = sizeof(" *)(") - 1; char *tb_c726r = tb_mem(tb_c726_0n + tb_c726_1n + tb_c726_2n + 1), *tb_c726rw = tb_c726r; memcpy(tb_c726rw, tb_c726_0, tb_c726_0n); tb_c726rw += tb_c726_0n; memcpy(tb_c726rw, tb_c726_1, tb_c726_1n); tb_c726rw += tb_c726_1n; memcpy(tb_c726rw, tb_c726_2, tb_c726_2n); tb_c726rw += tb_c726_2n; *tb_c726rw = 0; (const char *)tb_c726r; }));
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, ")->vals->data)");
            return;
        }
        if ((({ const char *tb_s727 = ((tb_o_Node *)tb_nn(v_n, 297))->q_name; ((unsigned char)tb_s727[0] == 118 && (unsigned char)tb_s727[1] == 97 && (unsigned char)tb_s727[2] == 108 && (unsigned char)tb_s727[3] == 117 && (unsigned char)tb_s727[4] == 101 && (unsigned char)tb_s727[5] == 115 && tb_s727[6] == 0); }) && f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_n, 297))->q_ty))) {
            f_emit__put(v_c, "tb_bits_of_bytes((");
            f_emit__emit_expr(v_c, v_base);
            f_emit__put(v_c, ")->vals)");
            return;
        }
        if ((({ const char *tb_s728 = ((tb_o_Node *)tb_nn(v_n, 302))->q_name; ((unsigned char)tb_s728[0] == 107 && (unsigned char)tb_s728[1] == 101 && (unsigned char)tb_s728[2] == 121 && (unsigned char)tb_s728[3] == 115 && tb_s728[4] == 0); }) || ({ const char *tb_s729 = ((tb_o_Node *)tb_nn(v_n, 302))->q_name; ((unsigned char)tb_s729[0] == 118 && (unsigned char)tb_s729[1] == 97 && (unsigned char)tb_s729[2] == 108 && (unsigned char)tb_s729[3] == 117 && (unsigned char)tb_s729[4] == 101 && (unsigned char)tb_s729[5] == 115 && tb_s729[6] == 0); }))) {
            f_emit__put(v_c, "tb_copy((");
            f_emit__emit_expr(v_c, v_base);
            if (({ const char *tb_s730 = ((tb_o_Node *)tb_nn(v_n, 305))->q_name; ((unsigned char)tb_s730[0] == 107 && (unsigned char)tb_s730[1] == 101 && (unsigned char)tb_s730[2] == 121 && (unsigned char)tb_s730[3] == 115 && tb_s730[4] == 0); })) {
                f_emit__put(v_c, ")->keys)");
            } else {
                f_emit__put(v_c, ")->vals)");
            }
            return;
        }
        if (({ const char *tb_s731 = ((tb_o_Node *)tb_nn(v_n, 310))->q_name; ((unsigned char)tb_s731[0] == 104 && (unsigned char)tb_s731[1] == 97 && (unsigned char)tb_s731[2] == 115 && tb_s731[3] == 0); })) {
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
        f_emit__put(v_c, ({ const char *tb_c732_0 = ", "; long long tb_c732_0n = sizeof(", ") - 1; char tb_c732_1d[24]; long long tb_c732_1n; const char *tb_c732_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 322))->q_line, tb_c732_1d, &tb_c732_1n); const char *tb_c732_2 = ")"; long long tb_c732_2n = sizeof(")") - 1; char *tb_c732r = tb_mem(tb_c732_0n + tb_c732_1n + tb_c732_2n + 1), *tb_c732rw = tb_c732r; memcpy(tb_c732rw, tb_c732_0, tb_c732_0n); tb_c732rw += tb_c732_0n; memcpy(tb_c732rw, tb_c732_1, tb_c732_1n); tb_c732rw += tb_c732_1n; memcpy(tb_c732rw, tb_c732_2, tb_c732_2n); tb_c732rw += tb_c732_2n; *tb_c732rw = 0; (const char *)tb_c732r; }));
        return;
    }
    f_emit__put(v_c, ({ const char *tb_c733_0 = "(*("; long long tb_c733_0n = sizeof("(*(") - 1; const char *tb_c733_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 324))->q_ty); long long tb_c733_1n = (long long)strlen(tb_c733_1); const char *tb_c733_2 = " *)tb_pop("; long long tb_c733_2n = sizeof(" *)tb_pop(") - 1; char *tb_c733r = tb_mem(tb_c733_0n + tb_c733_1n + tb_c733_2n + 1), *tb_c733rw = tb_c733r; memcpy(tb_c733rw, tb_c733_0, tb_c733_0n); tb_c733rw += tb_c733_0n; memcpy(tb_c733rw, tb_c733_1, tb_c733_1n); tb_c733rw += tb_c733_1n; memcpy(tb_c733rw, tb_c733_2, tb_c733_2n); tb_c733rw += tb_c733_2n; *tb_c733rw = 0; (const char *)tb_c733r; }));
    f_emit__emit_expr(v_c, v_base);
    f_emit__put(v_c, ({ const char *tb_c734_0 = ", "; long long tb_c734_0n = sizeof(", ") - 1; char tb_c734_1d[24]; long long tb_c734_1n; const char *tb_c734_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 326))->q_line, tb_c734_1d, &tb_c734_1n); const char *tb_c734_2 = "))"; long long tb_c734_2n = sizeof("))") - 1; char *tb_c734r = tb_mem(tb_c734_0n + tb_c734_1n + tb_c734_2n + 1), *tb_c734rw = tb_c734r; memcpy(tb_c734rw, tb_c734_0, tb_c734_0n); tb_c734rw += tb_c734_0n; memcpy(tb_c734rw, tb_c734_1, tb_c734_1n); tb_c734rw += tb_c734_1n; memcpy(tb_c734rw, tb_c734_2, tb_c734_2n); tb_c734rw += tb_c734_2n; *tb_c734rw = 0; (const char *)tb_c734r; }));
}

static void f_emit__emit_list(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 329))->q_types, ((tb_o_Node *)tb_nn(v_n, 329))->q_ty, sizeof(tb_o_Type *), 329)), 329))->q_elem;
    const char * v_elem = f_emit__c_type(v_c, v_elem_ty);
    if (f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn(v_n, 331))->q_ty)) {
        f_emit__put(v_c, ({ const char *tb_c735_0 = "tb_colsfrom_"; long long tb_c735_0n = sizeof("tb_colsfrom_") - 1; const char *tb_c735_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn(v_n, 332))->q_ty); long long tb_c735_1n = (long long)strlen(tb_c735_1); const char *tb_c735_2 = "("; long long tb_c735_2n = sizeof("(") - 1; char tb_c735_3d[24]; long long tb_c735_3n; const char *tb_c735_3 = tb_digits((((tb_o_Node *)tb_nn(v_n, 332))->q_kids)->len, tb_c735_3d, &tb_c735_3n); const char *tb_c735_4 = ", ("; long long tb_c735_4n = sizeof(", (") - 1; const char *tb_c735_5 = v_elem; long long tb_c735_5n = (long long)strlen(tb_c735_5); const char *tb_c735_6 = "[]){"; long long tb_c735_6n = sizeof("[]){") - 1; char *tb_c735r = tb_mem(tb_c735_0n + tb_c735_1n + tb_c735_2n + tb_c735_3n + tb_c735_4n + tb_c735_5n + tb_c735_6n + 1), *tb_c735rw = tb_c735r; memcpy(tb_c735rw, tb_c735_0, tb_c735_0n); tb_c735rw += tb_c735_0n; memcpy(tb_c735rw, tb_c735_1, tb_c735_1n); tb_c735rw += tb_c735_1n; memcpy(tb_c735rw, tb_c735_2, tb_c735_2n); tb_c735rw += tb_c735_2n; memcpy(tb_c735rw, tb_c735_3, tb_c735_3n); tb_c735rw += tb_c735_3n; memcpy(tb_c735rw, tb_c735_4, tb_c735_4n); tb_c735rw += tb_c735_4n; memcpy(tb_c735rw, tb_c735_5, tb_c735_5n); tb_c735rw += tb_c735_5n; memcpy(tb_c735rw, tb_c735_6, tb_c735_6n); tb_c735rw += tb_c735_6n; *tb_c735rw = 0; (const char *)tb_c735r; }));
    } else if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn(v_n, 333))->q_ty)) {
        f_emit__put(v_c, ({ const char *tb_c736_0 = "tb_bits_from("; long long tb_c736_0n = sizeof("tb_bits_from(") - 1; char tb_c736_1d[24]; long long tb_c736_1n; const char *tb_c736_1 = tb_digits((((tb_o_Node *)tb_nn(v_n, 334))->q_kids)->len, tb_c736_1d, &tb_c736_1n); const char *tb_c736_2 = ", (unsigned char[]){"; long long tb_c736_2n = sizeof(", (unsigned char[]){") - 1; char *tb_c736r = tb_mem(tb_c736_0n + tb_c736_1n + tb_c736_2n + 1), *tb_c736rw = tb_c736r; memcpy(tb_c736rw, tb_c736_0, tb_c736_0n); tb_c736rw += tb_c736_0n; memcpy(tb_c736rw, tb_c736_1, tb_c736_1n); tb_c736rw += tb_c736_1n; memcpy(tb_c736rw, tb_c736_2, tb_c736_2n); tb_c736rw += tb_c736_2n; *tb_c736rw = 0; (const char *)tb_c736r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c737_0 = "tb_list_from("; long long tb_c737_0n = sizeof("tb_list_from(") - 1; char tb_c737_1d[24]; long long tb_c737_1n; const char *tb_c737_1 = tb_digits((((tb_o_Node *)tb_nn(v_n, 336))->q_kids)->len, tb_c737_1d, &tb_c737_1n); const char *tb_c737_2 = ", sizeof("; long long tb_c737_2n = sizeof(", sizeof(") - 1; const char *tb_c737_3 = v_elem; long long tb_c737_3n = (long long)strlen(tb_c737_3); const char *tb_c737_4 = "), ("; long long tb_c737_4n = sizeof("), (") - 1; const char *tb_c737_5 = v_elem; long long tb_c737_5n = (long long)strlen(tb_c737_5); const char *tb_c737_6 = "[]){"; long long tb_c737_6n = sizeof("[]){") - 1; char *tb_c737r = tb_mem(tb_c737_0n + tb_c737_1n + tb_c737_2n + tb_c737_3n + tb_c737_4n + tb_c737_5n + tb_c737_6n + 1), *tb_c737rw = tb_c737r; memcpy(tb_c737rw, tb_c737_0, tb_c737_0n); tb_c737rw += tb_c737_0n; memcpy(tb_c737rw, tb_c737_1, tb_c737_1n); tb_c737rw += tb_c737_1n; memcpy(tb_c737rw, tb_c737_2, tb_c737_2n); tb_c737rw += tb_c737_2n; memcpy(tb_c737rw, tb_c737_3, tb_c737_3n); tb_c737rw += tb_c737_3n; memcpy(tb_c737rw, tb_c737_4, tb_c737_4n); tb_c737rw += tb_c737_4n; memcpy(tb_c737rw, tb_c737_5, tb_c737_5n); tb_c737rw += tb_c737_5n; memcpy(tb_c737rw, tb_c737_6, tb_c737_6n); tb_c737rw += tb_c737_6n; *tb_c737rw = 0; (const char *)tb_c737r; }));
    }
    {
        long long tb_end738 = ((((tb_o_Node *)tb_nn(v_n, 337))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end738; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 340))->q_kids, v_i, sizeof(tb_o_Node *), 340)), v_elem_ty);
        }
    }
    f_emit__put(v_c, "})");
}

static void f_emit__concat_parts(tb_o_Comp * v_c, tb_o_Node * v_n, tb_list * v_parts) {
    if (((({ const char *tb_s739 = ((tb_o_Node *)tb_nn(v_n, 345))->q_kind; ((unsigned char)tb_s739[0] == 98 && (unsigned char)tb_s739[1] == 105 && (unsigned char)tb_s739[2] == 110 && (unsigned char)tb_s739[3] == 111 && (unsigned char)tb_s739[4] == 112 && tb_s739[5] == 0); }) && ({ const char *tb_s740 = ((tb_o_Node *)tb_nn(v_n, 345))->q_value; ((unsigned char)tb_s740[0] == 43 && tb_s740[1] == 0); })) && ({ const char *tb_s741 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 345))->q_ty); ((unsigned char)tb_s741[0] == 116 && (unsigned char)tb_s741[1] == 101 && (unsigned char)tb_s741[2] == 120 && (unsigned char)tb_s741[3] == 116 && tb_s741[4] == 0); }))) {
        f_emit__concat_parts(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 346))->q_kids, 0LL, sizeof(tb_o_Node *), 346)), v_parts);
        f_emit__concat_parts(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 347))->q_kids, 1LL, sizeof(tb_o_Node *), 347)), v_parts);
    } else {
        { tb_o_Node * tb_v742 = v_n; *(tb_o_Node * *)tb_grow(v_parts) = tb_v742; }
    }
}

static void f_emit__emit_concat(tb_o_Comp * v_c, tb_o_Node * v_n) {
    long long tb_rl_total = -1, tb_rc_total = 0; (void)tb_rl_total; (void)tb_rc_total;
    tb_list * v_parts = tb_list_new(sizeof(tb_o_Node *));
    f_emit__concat_parts(v_c, v_n, v_parts);
    { long long tb_v743 = 1LL; long long *tb_p743 = &(((tb_o_Comp *)tb_nn(v_c, 357))->q_label); *tb_p743 += tb_v743; }
    const char * v_id = ({ const char *tb_c744_0 = ""; long long tb_c744_0n = sizeof("") - 1; char tb_c744_1d[24]; long long tb_c744_1n; const char *tb_c744_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 358))->q_label, tb_c744_1d, &tb_c744_1n); char *tb_c744r = tb_mem(tb_c744_0n + tb_c744_1n + 1), *tb_c744rw = tb_c744r; memcpy(tb_c744rw, tb_c744_0, tb_c744_0n); tb_c744rw += tb_c744_0n; memcpy(tb_c744rw, tb_c744_1, tb_c744_1n); tb_c744rw += tb_c744_1n; *tb_c744rw = 0; (const char *)tb_c744r; });
    f_emit__put(v_c, "({ ");
    const char * v_total = ""; tb_rl_total = -1;
    {
        long long tb_end745 = ((v_parts)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end745; v_i++) {
            tb_o_Node * v_part = (*(tb_o_Node * *)tb_at(v_parts, v_i, sizeof(tb_o_Node *), 362));
            const char * v_v = ({ const char *tb_c746_0 = "tb_c"; long long tb_c746_0n = sizeof("tb_c") - 1; const char *tb_c746_1 = v_id; long long tb_c746_1n = (long long)strlen(tb_c746_1); const char *tb_c746_2 = "_"; long long tb_c746_2n = sizeof("_") - 1; char tb_c746_3d[24]; long long tb_c746_3n; const char *tb_c746_3 = tb_digits(v_i, tb_c746_3d, &tb_c746_3n); char *tb_c746r = tb_mem(tb_c746_0n + tb_c746_1n + tb_c746_2n + tb_c746_3n + 1), *tb_c746rw = tb_c746r; memcpy(tb_c746rw, tb_c746_0, tb_c746_0n); tb_c746rw += tb_c746_0n; memcpy(tb_c746rw, tb_c746_1, tb_c746_1n); tb_c746rw += tb_c746_1n; memcpy(tb_c746rw, tb_c746_2, tb_c746_2n); tb_c746rw += tb_c746_2n; memcpy(tb_c746rw, tb_c746_3, tb_c746_3n); tb_c746rw += tb_c746_3n; *tb_c746rw = 0; (const char *)tb_c746r; });
            const char * v_k = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_part, 364))->q_ty);
            if (({ const char *tb_s747 = v_k; ((unsigned char)tb_s747[0] == 105 && (unsigned char)tb_s747[1] == 110 && (unsigned char)tb_s747[2] == 116 && tb_s747[3] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c748_0 = "char "; long long tb_c748_0n = sizeof("char ") - 1; const char *tb_c748_1 = v_v; long long tb_c748_1n = (long long)strlen(tb_c748_1); const char *tb_c748_2 = "d[24]; long long "; long long tb_c748_2n = sizeof("d[24]; long long ") - 1; const char *tb_c748_3 = v_v; long long tb_c748_3n = (long long)strlen(tb_c748_3); const char *tb_c748_4 = "n; const char *"; long long tb_c748_4n = sizeof("n; const char *") - 1; const char *tb_c748_5 = v_v; long long tb_c748_5n = (long long)strlen(tb_c748_5); const char *tb_c748_6 = " = tb_digits("; long long tb_c748_6n = sizeof(" = tb_digits(") - 1; char *tb_c748r = tb_mem(tb_c748_0n + tb_c748_1n + tb_c748_2n + tb_c748_3n + tb_c748_4n + tb_c748_5n + tb_c748_6n + 1), *tb_c748rw = tb_c748r; memcpy(tb_c748rw, tb_c748_0, tb_c748_0n); tb_c748rw += tb_c748_0n; memcpy(tb_c748rw, tb_c748_1, tb_c748_1n); tb_c748rw += tb_c748_1n; memcpy(tb_c748rw, tb_c748_2, tb_c748_2n); tb_c748rw += tb_c748_2n; memcpy(tb_c748rw, tb_c748_3, tb_c748_3n); tb_c748rw += tb_c748_3n; memcpy(tb_c748rw, tb_c748_4, tb_c748_4n); tb_c748rw += tb_c748_4n; memcpy(tb_c748rw, tb_c748_5, tb_c748_5n); tb_c748rw += tb_c748_5n; memcpy(tb_c748rw, tb_c748_6, tb_c748_6n); tb_c748rw += tb_c748_6n; *tb_c748rw = 0; (const char *)tb_c748r; }));
                f_emit__emit_expr(v_c, v_part);
                f_emit__put(v_c, ({ const char *tb_c749_0 = ", "; long long tb_c749_0n = sizeof(", ") - 1; const char *tb_c749_1 = v_v; long long tb_c749_1n = (long long)strlen(tb_c749_1); const char *tb_c749_2 = "d, &"; long long tb_c749_2n = sizeof("d, &") - 1; const char *tb_c749_3 = v_v; long long tb_c749_3n = (long long)strlen(tb_c749_3); const char *tb_c749_4 = "n); "; long long tb_c749_4n = sizeof("n); ") - 1; char *tb_c749r = tb_mem(tb_c749_0n + tb_c749_1n + tb_c749_2n + tb_c749_3n + tb_c749_4n + 1), *tb_c749rw = tb_c749r; memcpy(tb_c749rw, tb_c749_0, tb_c749_0n); tb_c749rw += tb_c749_0n; memcpy(tb_c749rw, tb_c749_1, tb_c749_1n); tb_c749rw += tb_c749_1n; memcpy(tb_c749rw, tb_c749_2, tb_c749_2n); tb_c749rw += tb_c749_2n; memcpy(tb_c749rw, tb_c749_3, tb_c749_3n); tb_c749rw += tb_c749_3n; memcpy(tb_c749rw, tb_c749_4, tb_c749_4n); tb_c749rw += tb_c749_4n; *tb_c749rw = 0; (const char *)tb_c749r; }));
            } else if (({ const char *tb_s750 = ((tb_o_Node *)tb_nn(v_part, 369))->q_kind; ((unsigned char)tb_s750[0] == 115 && (unsigned char)tb_s750[1] == 116 && (unsigned char)tb_s750[2] == 114 && (unsigned char)tb_s750[3] == 105 && (unsigned char)tb_s750[4] == 110 && (unsigned char)tb_s750[5] == 103 && tb_s750[6] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c751_0 = "const char *"; long long tb_c751_0n = sizeof("const char *") - 1; const char *tb_c751_1 = v_v; long long tb_c751_1n = (long long)strlen(tb_c751_1); const char *tb_c751_2 = " = "; long long tb_c751_2n = sizeof(" = ") - 1; const char *tb_c751_3 = ((tb_o_Node *)tb_nn(v_part, 370))->q_value; long long tb_c751_3n = (long long)strlen(tb_c751_3); const char *tb_c751_4 = "; long long "; long long tb_c751_4n = sizeof("; long long ") - 1; const char *tb_c751_5 = v_v; long long tb_c751_5n = (long long)strlen(tb_c751_5); const char *tb_c751_6 = "n = sizeof("; long long tb_c751_6n = sizeof("n = sizeof(") - 1; const char *tb_c751_7 = ((tb_o_Node *)tb_nn(v_part, 370))->q_value; long long tb_c751_7n = (long long)strlen(tb_c751_7); const char *tb_c751_8 = ") - 1; "; long long tb_c751_8n = sizeof(") - 1; ") - 1; char *tb_c751r = tb_mem(tb_c751_0n + tb_c751_1n + tb_c751_2n + tb_c751_3n + tb_c751_4n + tb_c751_5n + tb_c751_6n + tb_c751_7n + tb_c751_8n + 1), *tb_c751rw = tb_c751r; memcpy(tb_c751rw, tb_c751_0, tb_c751_0n); tb_c751rw += tb_c751_0n; memcpy(tb_c751rw, tb_c751_1, tb_c751_1n); tb_c751rw += tb_c751_1n; memcpy(tb_c751rw, tb_c751_2, tb_c751_2n); tb_c751rw += tb_c751_2n; memcpy(tb_c751rw, tb_c751_3, tb_c751_3n); tb_c751rw += tb_c751_3n; memcpy(tb_c751rw, tb_c751_4, tb_c751_4n); tb_c751rw += tb_c751_4n; memcpy(tb_c751rw, tb_c751_5, tb_c751_5n); tb_c751rw += tb_c751_5n; memcpy(tb_c751rw, tb_c751_6, tb_c751_6n); tb_c751rw += tb_c751_6n; memcpy(tb_c751rw, tb_c751_7, tb_c751_7n); tb_c751rw += tb_c751_7n; memcpy(tb_c751rw, tb_c751_8, tb_c751_8n); tb_c751rw += tb_c751_8n; *tb_c751rw = 0; (const char *)tb_c751r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c752_0 = "const char *"; long long tb_c752_0n = sizeof("const char *") - 1; const char *tb_c752_1 = v_v; long long tb_c752_1n = (long long)strlen(tb_c752_1); const char *tb_c752_2 = " = "; long long tb_c752_2n = sizeof(" = ") - 1; char *tb_c752r = tb_mem(tb_c752_0n + tb_c752_1n + tb_c752_2n + 1), *tb_c752rw = tb_c752r; memcpy(tb_c752rw, tb_c752_0, tb_c752_0n); tb_c752rw += tb_c752_0n; memcpy(tb_c752rw, tb_c752_1, tb_c752_1n); tb_c752rw += tb_c752_1n; memcpy(tb_c752rw, tb_c752_2, tb_c752_2n); tb_c752rw += tb_c752_2n; *tb_c752rw = 0; (const char *)tb_c752r; }));
                f_emit__emit_as_text(v_c, v_part);
                f_emit__put(v_c, ({ const char *tb_c753_0 = "; long long "; long long tb_c753_0n = sizeof("; long long ") - 1; const char *tb_c753_1 = v_v; long long tb_c753_1n = (long long)strlen(tb_c753_1); const char *tb_c753_2 = "n = (long long)strlen("; long long tb_c753_2n = sizeof("n = (long long)strlen(") - 1; const char *tb_c753_3 = v_v; long long tb_c753_3n = (long long)strlen(tb_c753_3); const char *tb_c753_4 = "); "; long long tb_c753_4n = sizeof("); ") - 1; char *tb_c753r = tb_mem(tb_c753_0n + tb_c753_1n + tb_c753_2n + tb_c753_3n + tb_c753_4n + 1), *tb_c753rw = tb_c753r; memcpy(tb_c753rw, tb_c753_0, tb_c753_0n); tb_c753rw += tb_c753_0n; memcpy(tb_c753rw, tb_c753_1, tb_c753_1n); tb_c753rw += tb_c753_1n; memcpy(tb_c753rw, tb_c753_2, tb_c753_2n); tb_c753rw += tb_c753_2n; memcpy(tb_c753rw, tb_c753_3, tb_c753_3n); tb_c753rw += tb_c753_3n; memcpy(tb_c753rw, tb_c753_4, tb_c753_4n); tb_c753rw += tb_c753_4n; *tb_c753rw = 0; (const char *)tb_c753r; }));
            }
            if ((v_i > 0LL)) {
                tb_app_n(&v_total, &tb_rl_total, &tb_rc_total, " + ", sizeof(" + ") - 1); 
            }
            tb_app(&v_total, &tb_rl_total, &tb_rc_total, v_v); tb_app_n(&v_total, &tb_rl_total, &tb_rc_total, "n", sizeof("n") - 1); 
        }
    }
    const char * v_r = ({ const char *tb_c754_0 = "tb_c"; long long tb_c754_0n = sizeof("tb_c") - 1; const char *tb_c754_1 = v_id; long long tb_c754_1n = (long long)strlen(tb_c754_1); const char *tb_c754_2 = "r"; long long tb_c754_2n = sizeof("r") - 1; char *tb_c754r = tb_mem(tb_c754_0n + tb_c754_1n + tb_c754_2n + 1), *tb_c754rw = tb_c754r; memcpy(tb_c754rw, tb_c754_0, tb_c754_0n); tb_c754rw += tb_c754_0n; memcpy(tb_c754rw, tb_c754_1, tb_c754_1n); tb_c754rw += tb_c754_1n; memcpy(tb_c754rw, tb_c754_2, tb_c754_2n); tb_c754rw += tb_c754_2n; *tb_c754rw = 0; (const char *)tb_c754r; });
    f_emit__put(v_c, ({ const char *tb_c755_0 = "char *"; long long tb_c755_0n = sizeof("char *") - 1; const char *tb_c755_1 = v_r; long long tb_c755_1n = (long long)strlen(tb_c755_1); const char *tb_c755_2 = " = tb_mem("; long long tb_c755_2n = sizeof(" = tb_mem(") - 1; const char *tb_c755_3 = v_total; long long tb_c755_3n = (long long)strlen(tb_c755_3); const char *tb_c755_4 = " + 1), *"; long long tb_c755_4n = sizeof(" + 1), *") - 1; const char *tb_c755_5 = v_r; long long tb_c755_5n = (long long)strlen(tb_c755_5); const char *tb_c755_6 = "w = "; long long tb_c755_6n = sizeof("w = ") - 1; const char *tb_c755_7 = v_r; long long tb_c755_7n = (long long)strlen(tb_c755_7); const char *tb_c755_8 = "; "; long long tb_c755_8n = sizeof("; ") - 1; char *tb_c755r = tb_mem(tb_c755_0n + tb_c755_1n + tb_c755_2n + tb_c755_3n + tb_c755_4n + tb_c755_5n + tb_c755_6n + tb_c755_7n + tb_c755_8n + 1), *tb_c755rw = tb_c755r; memcpy(tb_c755rw, tb_c755_0, tb_c755_0n); tb_c755rw += tb_c755_0n; memcpy(tb_c755rw, tb_c755_1, tb_c755_1n); tb_c755rw += tb_c755_1n; memcpy(tb_c755rw, tb_c755_2, tb_c755_2n); tb_c755rw += tb_c755_2n; memcpy(tb_c755rw, tb_c755_3, tb_c755_3n); tb_c755rw += tb_c755_3n; memcpy(tb_c755rw, tb_c755_4, tb_c755_4n); tb_c755rw += tb_c755_4n; memcpy(tb_c755rw, tb_c755_5, tb_c755_5n); tb_c755rw += tb_c755_5n; memcpy(tb_c755rw, tb_c755_6, tb_c755_6n); tb_c755rw += tb_c755_6n; memcpy(tb_c755rw, tb_c755_7, tb_c755_7n); tb_c755rw += tb_c755_7n; memcpy(tb_c755rw, tb_c755_8, tb_c755_8n); tb_c755rw += tb_c755_8n; *tb_c755rw = 0; (const char *)tb_c755r; }));
    {
        long long tb_end756 = ((v_parts)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end756; v_i++) {
            const char * v_v = ({ const char *tb_c757_0 = "tb_c"; long long tb_c757_0n = sizeof("tb_c") - 1; const char *tb_c757_1 = v_id; long long tb_c757_1n = (long long)strlen(tb_c757_1); const char *tb_c757_2 = "_"; long long tb_c757_2n = sizeof("_") - 1; char tb_c757_3d[24]; long long tb_c757_3n; const char *tb_c757_3 = tb_digits(v_i, tb_c757_3d, &tb_c757_3n); char *tb_c757r = tb_mem(tb_c757_0n + tb_c757_1n + tb_c757_2n + tb_c757_3n + 1), *tb_c757rw = tb_c757r; memcpy(tb_c757rw, tb_c757_0, tb_c757_0n); tb_c757rw += tb_c757_0n; memcpy(tb_c757rw, tb_c757_1, tb_c757_1n); tb_c757rw += tb_c757_1n; memcpy(tb_c757rw, tb_c757_2, tb_c757_2n); tb_c757rw += tb_c757_2n; memcpy(tb_c757rw, tb_c757_3, tb_c757_3n); tb_c757rw += tb_c757_3n; *tb_c757rw = 0; (const char *)tb_c757r; });
            f_emit__put(v_c, ({ const char *tb_c758_0 = "memcpy("; long long tb_c758_0n = sizeof("memcpy(") - 1; const char *tb_c758_1 = v_r; long long tb_c758_1n = (long long)strlen(tb_c758_1); const char *tb_c758_2 = "w, "; long long tb_c758_2n = sizeof("w, ") - 1; const char *tb_c758_3 = v_v; long long tb_c758_3n = (long long)strlen(tb_c758_3); const char *tb_c758_4 = ", "; long long tb_c758_4n = sizeof(", ") - 1; const char *tb_c758_5 = v_v; long long tb_c758_5n = (long long)strlen(tb_c758_5); const char *tb_c758_6 = "n); "; long long tb_c758_6n = sizeof("n); ") - 1; const char *tb_c758_7 = v_r; long long tb_c758_7n = (long long)strlen(tb_c758_7); const char *tb_c758_8 = "w += "; long long tb_c758_8n = sizeof("w += ") - 1; const char *tb_c758_9 = v_v; long long tb_c758_9n = (long long)strlen(tb_c758_9); const char *tb_c758_10 = "n; "; long long tb_c758_10n = sizeof("n; ") - 1; char *tb_c758r = tb_mem(tb_c758_0n + tb_c758_1n + tb_c758_2n + tb_c758_3n + tb_c758_4n + tb_c758_5n + tb_c758_6n + tb_c758_7n + tb_c758_8n + tb_c758_9n + tb_c758_10n + 1), *tb_c758rw = tb_c758r; memcpy(tb_c758rw, tb_c758_0, tb_c758_0n); tb_c758rw += tb_c758_0n; memcpy(tb_c758rw, tb_c758_1, tb_c758_1n); tb_c758rw += tb_c758_1n; memcpy(tb_c758rw, tb_c758_2, tb_c758_2n); tb_c758rw += tb_c758_2n; memcpy(tb_c758rw, tb_c758_3, tb_c758_3n); tb_c758rw += tb_c758_3n; memcpy(tb_c758rw, tb_c758_4, tb_c758_4n); tb_c758rw += tb_c758_4n; memcpy(tb_c758rw, tb_c758_5, tb_c758_5n); tb_c758rw += tb_c758_5n; memcpy(tb_c758rw, tb_c758_6, tb_c758_6n); tb_c758rw += tb_c758_6n; memcpy(tb_c758rw, tb_c758_7, tb_c758_7n); tb_c758rw += tb_c758_7n; memcpy(tb_c758rw, tb_c758_8, tb_c758_8n); tb_c758rw += tb_c758_8n; memcpy(tb_c758rw, tb_c758_9, tb_c758_9n); tb_c758rw += tb_c758_9n; memcpy(tb_c758rw, tb_c758_10, tb_c758_10n); tb_c758rw += tb_c758_10n; *tb_c758rw = 0; (const char *)tb_c758r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c759_0 = "*"; long long tb_c759_0n = sizeof("*") - 1; const char *tb_c759_1 = v_r; long long tb_c759_1n = (long long)strlen(tb_c759_1); const char *tb_c759_2 = "w = 0; (const char *)"; long long tb_c759_2n = sizeof("w = 0; (const char *)") - 1; const char *tb_c759_3 = v_r; long long tb_c759_3n = (long long)strlen(tb_c759_3); const char *tb_c759_4 = "; })"; long long tb_c759_4n = sizeof("; })") - 1; char *tb_c759r = tb_mem(tb_c759_0n + tb_c759_1n + tb_c759_2n + tb_c759_3n + tb_c759_4n + 1), *tb_c759rw = tb_c759r; memcpy(tb_c759rw, tb_c759_0, tb_c759_0n); tb_c759rw += tb_c759_0n; memcpy(tb_c759rw, tb_c759_1, tb_c759_1n); tb_c759rw += tb_c759_1n; memcpy(tb_c759rw, tb_c759_2, tb_c759_2n); tb_c759rw += tb_c759_2n; memcpy(tb_c759rw, tb_c759_3, tb_c759_3n); tb_c759rw += tb_c759_3n; memcpy(tb_c759rw, tb_c759_4, tb_c759_4n); tb_c759rw += tb_c759_4n; *tb_c759rw = 0; (const char *)tb_c759r; }));
}

static void f_emit__emit_binop(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 386))->q_value;
    tb_o_Node * v_a = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 387))->q_kids, 0LL, sizeof(tb_o_Node *), 387));
    tb_o_Node * v_b = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 388))->q_kids, 1LL, sizeof(tb_o_Node *), 388));
    if (({ const char *tb_s760 = v_op; ((unsigned char)tb_s760[0] == 111 && (unsigned char)tb_s760[1] == 114 && (unsigned char)tb_s760[2] == 101 && (unsigned char)tb_s760[3] == 108 && (unsigned char)tb_s760[4] == 115 && (unsigned char)tb_s760[5] == 101 && tb_s760[6] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c761_0 = "(tb_t"; long long tb_c761_0n = sizeof("(tb_t") - 1; char tb_c761_1d[24]; long long tb_c761_1n; const char *tb_c761_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 391))->q_temp_no, tb_c761_1d, &tb_c761_1n); const char *tb_c761_2 = " = "; long long tb_c761_2n = sizeof(" = ") - 1; char *tb_c761r = tb_mem(tb_c761_0n + tb_c761_1n + tb_c761_2n + 1), *tb_c761rw = tb_c761r; memcpy(tb_c761rw, tb_c761_0, tb_c761_0n); tb_c761rw += tb_c761_0n; memcpy(tb_c761rw, tb_c761_1, tb_c761_1n); tb_c761rw += tb_c761_1n; memcpy(tb_c761rw, tb_c761_2, tb_c761_2n); tb_c761rw += tb_c761_2n; *tb_c761rw = 0; (const char *)tb_c761r; }));
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ", tb_err ? (tb_err = 0, ");
        f_emit__emit_as(v_c, v_b, ((tb_o_Node *)tb_nn(v_n, 394))->q_ty);
        f_emit__put(v_c, ({ const char *tb_c762_0 = ") : tb_t"; long long tb_c762_0n = sizeof(") : tb_t") - 1; char tb_c762_1d[24]; long long tb_c762_1n; const char *tb_c762_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 395))->q_temp_no, tb_c762_1d, &tb_c762_1n); const char *tb_c762_2 = ")"; long long tb_c762_2n = sizeof(")") - 1; char *tb_c762r = tb_mem(tb_c762_0n + tb_c762_1n + tb_c762_2n + 1), *tb_c762rw = tb_c762r; memcpy(tb_c762rw, tb_c762_0, tb_c762_0n); tb_c762rw += tb_c762_0n; memcpy(tb_c762rw, tb_c762_1, tb_c762_1n); tb_c762rw += tb_c762_1n; memcpy(tb_c762rw, tb_c762_2, tb_c762_2n); tb_c762rw += tb_c762_2n; *tb_c762rw = 0; (const char *)tb_c762r; }));
        return;
    }
    if (({ const char *tb_s763 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 397))->q_ty); ((unsigned char)tb_s763[0] == 116 && (unsigned char)tb_s763[1] == 101 && (unsigned char)tb_s763[2] == 120 && (unsigned char)tb_s763[3] == 116 && tb_s763[4] == 0); })) {
        f_emit__emit_concat(v_c, v_n);
        return;
    }
    if (((({ const char *tb_s764 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 400))->q_ty); ((unsigned char)tb_s764[0] == 116 && (unsigned char)tb_s764[1] == 101 && (unsigned char)tb_s764[2] == 120 && (unsigned char)tb_s764[3] == 116 && tb_s764[4] == 0); }) && (({ const char *tb_s765 = v_op; ((unsigned char)tb_s765[0] == 61 && (unsigned char)tb_s765[1] == 61 && tb_s765[2] == 0); }) || ({ const char *tb_s766 = v_op; ((unsigned char)tb_s766[0] == 33 && (unsigned char)tb_s766[1] == 61 && tb_s766[2] == 0); }))) && (f_emit__short_literal(v_b) || f_emit__short_literal(v_a)))) {
        tb_o_Node * v_lit = v_b;
        tb_o_Node * v_other = v_a;
        if ((!f_emit__short_literal(v_b))) {
            v_lit = v_a;
            v_other = v_b;
        }
        const char * v_body = tb_cut(((tb_o_Node *)tb_nn(v_lit, 407))->q_value, 1LL, (tb_tlen(((tb_o_Node *)tb_nn(v_lit, 407))->q_value) - 2LL), 407);
        { long long tb_v767 = 1LL; long long *tb_p767 = &(((tb_o_Comp *)tb_nn(v_c, 408))->q_label); *tb_p767 += tb_v767; }
        const char * v_sv = ({ const char *tb_c768_0 = "tb_s"; long long tb_c768_0n = sizeof("tb_s") - 1; char tb_c768_1d[24]; long long tb_c768_1n; const char *tb_c768_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 409))->q_label, tb_c768_1d, &tb_c768_1n); char *tb_c768r = tb_mem(tb_c768_0n + tb_c768_1n + 1), *tb_c768rw = tb_c768r; memcpy(tb_c768rw, tb_c768_0, tb_c768_0n); tb_c768rw += tb_c768_0n; memcpy(tb_c768rw, tb_c768_1, tb_c768_1n); tb_c768rw += tb_c768_1n; *tb_c768rw = 0; (const char *)tb_c768r; });
        f_emit__put(v_c, ({ const char *tb_c769_0 = "({ const char *"; long long tb_c769_0n = sizeof("({ const char *") - 1; const char *tb_c769_1 = v_sv; long long tb_c769_1n = (long long)strlen(tb_c769_1); const char *tb_c769_2 = " = "; long long tb_c769_2n = sizeof(" = ") - 1; char *tb_c769r = tb_mem(tb_c769_0n + tb_c769_1n + tb_c769_2n + 1), *tb_c769rw = tb_c769r; memcpy(tb_c769rw, tb_c769_0, tb_c769_0n); tb_c769rw += tb_c769_0n; memcpy(tb_c769rw, tb_c769_1, tb_c769_1n); tb_c769rw += tb_c769_1n; memcpy(tb_c769rw, tb_c769_2, tb_c769_2n); tb_c769rw += tb_c769_2n; *tb_c769rw = 0; (const char *)tb_c769r; }));
        f_emit__emit_q(v_c, v_other);
        f_emit__put(v_c, "; ");
        if (({ const char *tb_s770 = v_op; ((unsigned char)tb_s770[0] == 33 && (unsigned char)tb_s770[1] == 61 && tb_s770[2] == 0); })) {
            f_emit__put(v_c, "!");
        }
        f_emit__put(v_c, "(");
        long long v_i = 0LL;
        {
            const char * tb_l771 = v_body;
            for (long long tb_i771 = 0; tb_l771[tb_i771] != 0; tb_i771++) {
                const char * const v_ch = tb_chr((unsigned char)tb_l771[tb_i771]);
                f_emit__put(v_c, ({ const char *tb_c772_0 = "(unsigned char)"; long long tb_c772_0n = sizeof("(unsigned char)") - 1; const char *tb_c772_1 = v_sv; long long tb_c772_1n = (long long)strlen(tb_c772_1); const char *tb_c772_2 = "["; long long tb_c772_2n = sizeof("[") - 1; char tb_c772_3d[24]; long long tb_c772_3n; const char *tb_c772_3 = tb_digits(v_i, tb_c772_3d, &tb_c772_3n); const char *tb_c772_4 = "] == "; long long tb_c772_4n = sizeof("] == ") - 1; char tb_c772_5d[24]; long long tb_c772_5n; const char *tb_c772_5 = tb_digits(tb_code(v_ch, 418), tb_c772_5d, &tb_c772_5n); const char *tb_c772_6 = " && "; long long tb_c772_6n = sizeof(" && ") - 1; char *tb_c772r = tb_mem(tb_c772_0n + tb_c772_1n + tb_c772_2n + tb_c772_3n + tb_c772_4n + tb_c772_5n + tb_c772_6n + 1), *tb_c772rw = tb_c772r; memcpy(tb_c772rw, tb_c772_0, tb_c772_0n); tb_c772rw += tb_c772_0n; memcpy(tb_c772rw, tb_c772_1, tb_c772_1n); tb_c772rw += tb_c772_1n; memcpy(tb_c772rw, tb_c772_2, tb_c772_2n); tb_c772rw += tb_c772_2n; memcpy(tb_c772rw, tb_c772_3, tb_c772_3n); tb_c772rw += tb_c772_3n; memcpy(tb_c772rw, tb_c772_4, tb_c772_4n); tb_c772rw += tb_c772_4n; memcpy(tb_c772rw, tb_c772_5, tb_c772_5n); tb_c772rw += tb_c772_5n; memcpy(tb_c772rw, tb_c772_6, tb_c772_6n); tb_c772rw += tb_c772_6n; *tb_c772rw = 0; (const char *)tb_c772r; }));
                { long long tb_v773 = 1LL; long long *tb_p773 = &(v_i); *tb_p773 += tb_v773; }
            }
        }
        f_emit__put(v_c, ({ const char *tb_c774_0 = v_sv; long long tb_c774_0n = (long long)strlen(tb_c774_0); const char *tb_c774_1 = "["; long long tb_c774_1n = sizeof("[") - 1; char tb_c774_2d[24]; long long tb_c774_2n; const char *tb_c774_2 = tb_digits(v_i, tb_c774_2d, &tb_c774_2n); const char *tb_c774_3 = "] == 0); })"; long long tb_c774_3n = sizeof("] == 0); })") - 1; char *tb_c774r = tb_mem(tb_c774_0n + tb_c774_1n + tb_c774_2n + tb_c774_3n + 1), *tb_c774rw = tb_c774r; memcpy(tb_c774rw, tb_c774_0, tb_c774_0n); tb_c774rw += tb_c774_0n; memcpy(tb_c774rw, tb_c774_1, tb_c774_1n); tb_c774rw += tb_c774_1n; memcpy(tb_c774rw, tb_c774_2, tb_c774_2n); tb_c774rw += tb_c774_2n; memcpy(tb_c774rw, tb_c774_3, tb_c774_3n); tb_c774rw += tb_c774_3n; *tb_c774rw = 0; (const char *)tb_c774r; }));
        return;
    }
    if (({ const char *tb_s775 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_a, 422))->q_ty); ((unsigned char)tb_s775[0] == 116 && (unsigned char)tb_s775[1] == 101 && (unsigned char)tb_s775[2] == 120 && (unsigned char)tb_s775[3] == 116 && tb_s775[4] == 0); })) {
        f_emit__put(v_c, "(strcmp(");
        f_emit__emit_q(v_c, v_a);
        f_emit__put(v_c, ", ");
        f_emit__emit_q(v_c, v_b);
        f_emit__put(v_c, ({ const char *tb_c776_0 = ") "; long long tb_c776_0n = sizeof(") ") - 1; const char *tb_c776_1 = v_op; long long tb_c776_1n = (long long)strlen(tb_c776_1); const char *tb_c776_2 = " 0)"; long long tb_c776_2n = sizeof(" 0)") - 1; char *tb_c776r = tb_mem(tb_c776_0n + tb_c776_1n + tb_c776_2n + 1), *tb_c776rw = tb_c776r; memcpy(tb_c776rw, tb_c776_0, tb_c776_0n); tb_c776rw += tb_c776_0n; memcpy(tb_c776rw, tb_c776_1, tb_c776_1n); tb_c776rw += tb_c776_1n; memcpy(tb_c776rw, tb_c776_2, tb_c776_2n); tb_c776rw += tb_c776_2n; *tb_c776rw = 0; (const char *)tb_c776r; }));
        return;
    }
    if ((({ const char *tb_s777 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 429))->q_ty); ((unsigned char)tb_s777[0] == 105 && (unsigned char)tb_s777[1] == 110 && (unsigned char)tb_s777[2] == 116 && tb_s777[3] == 0); }) && (({ const char *tb_s778 = v_op; ((unsigned char)tb_s778[0] == 47 && tb_s778[1] == 0); }) || ({ const char *tb_s779 = v_op; ((unsigned char)tb_s779[0] == 37 && tb_s779[1] == 0); })))) {
        if (({ const char *tb_s780 = v_op; ((unsigned char)tb_s780[0] == 47 && tb_s780[1] == 0); })) {
            f_emit__put(v_c, "tb_div(");
        } else {
            f_emit__put(v_c, "tb_mod(");
        }
        f_emit__emit_expr(v_c, v_a);
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, v_b);
        f_emit__put(v_c, ({ const char *tb_c781_0 = ", "; long long tb_c781_0n = sizeof(", ") - 1; char tb_c781_1d[24]; long long tb_c781_1n; const char *tb_c781_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 437))->q_line, tb_c781_1d, &tb_c781_1n); const char *tb_c781_2 = ")"; long long tb_c781_2n = sizeof(")") - 1; char *tb_c781r = tb_mem(tb_c781_0n + tb_c781_1n + tb_c781_2n + 1), *tb_c781rw = tb_c781r; memcpy(tb_c781rw, tb_c781_0, tb_c781_0n); tb_c781rw += tb_c781_0n; memcpy(tb_c781rw, tb_c781_1, tb_c781_1n); tb_c781rw += tb_c781_1n; memcpy(tb_c781rw, tb_c781_2, tb_c781_2n); tb_c781rw += tb_c781_2n; *tb_c781rw = 0; (const char *)tb_c781r; }));
        return;
    }
    const char * v_c_op = v_op;
    if (({ const char *tb_s782 = v_op; ((unsigned char)tb_s782[0] == 97 && (unsigned char)tb_s782[1] == 110 && (unsigned char)tb_s782[2] == 100 && tb_s782[3] == 0); })) {
        v_c_op = "&&";
    } else if (({ const char *tb_s783 = v_op; ((unsigned char)tb_s783[0] == 111 && (unsigned char)tb_s783[1] == 114 && tb_s783[2] == 0); })) {
        v_c_op = "||";
    }
    f_emit__put(v_c, "(");
    if ((({ const char *tb_s784 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 445))->q_ty); ((unsigned char)tb_s784[0] == 100 && (unsigned char)tb_s784[1] == 101 && (unsigned char)tb_s784[2] == 99 && tb_s784[3] == 0); }) && ({ const char *tb_s785 = v_op; ((unsigned char)tb_s785[0] == 47 && tb_s785[1] == 0); }))) {
        f_emit__emit_as(v_c, v_a, ((tb_o_Node *)tb_nn(v_n, 446))->q_ty);
    } else {
        f_emit__emit_expr(v_c, v_a);
    }
    f_emit__put(v_c, ({ const char *tb_c786_0 = " "; long long tb_c786_0n = sizeof(" ") - 1; const char *tb_c786_1 = v_c_op; long long tb_c786_1n = (long long)strlen(tb_c786_1); const char *tb_c786_2 = " "; long long tb_c786_2n = sizeof(" ") - 1; char *tb_c786r = tb_mem(tb_c786_0n + tb_c786_1n + tb_c786_2n + 1), *tb_c786rw = tb_c786r; memcpy(tb_c786rw, tb_c786_0, tb_c786_0n); tb_c786rw += tb_c786_0n; memcpy(tb_c786rw, tb_c786_1, tb_c786_1n); tb_c786rw += tb_c786_1n; memcpy(tb_c786rw, tb_c786_2, tb_c786_2n); tb_c786rw += tb_c786_2n; *tb_c786rw = 0; (const char *)tb_c786r; }));
    f_emit__emit_expr(v_c, v_b);
    f_emit__put(v_c, ")");
}

static void f_emit__emit_expr(tb_o_Comp * v_c, tb_o_Node * v_n) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 454))->q_kind;
    if (({ const char *tb_s787 = v_k; ((unsigned char)tb_s787[0] == 110 && (unsigned char)tb_s787[1] == 117 && (unsigned char)tb_s787[2] == 109 && (unsigned char)tb_s787[3] == 98 && (unsigned char)tb_s787[4] == 101 && (unsigned char)tb_s787[5] == 114 && tb_s787[6] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c788_0 = ((tb_o_Node *)tb_nn(v_n, 456))->q_value; long long tb_c788_0n = (long long)strlen(tb_c788_0); const char *tb_c788_1 = "LL"; long long tb_c788_1n = sizeof("LL") - 1; char *tb_c788r = tb_mem(tb_c788_0n + tb_c788_1n + 1), *tb_c788rw = tb_c788r; memcpy(tb_c788rw, tb_c788_0, tb_c788_0n); tb_c788rw += tb_c788_0n; memcpy(tb_c788rw, tb_c788_1, tb_c788_1n); tb_c788rw += tb_c788_1n; *tb_c788rw = 0; (const char *)tb_c788r; }));
    } else if ((({ const char *tb_s789 = v_k; ((unsigned char)tb_s789[0] == 100 && (unsigned char)tb_s789[1] == 101 && (unsigned char)tb_s789[2] == 99 && (unsigned char)tb_s789[3] == 105 && (unsigned char)tb_s789[4] == 109 && (unsigned char)tb_s789[5] == 97 && (unsigned char)tb_s789[6] == 108 && tb_s789[7] == 0); }) || ({ const char *tb_s790 = v_k; ((unsigned char)tb_s790[0] == 115 && (unsigned char)tb_s790[1] == 116 && (unsigned char)tb_s790[2] == 114 && (unsigned char)tb_s790[3] == 105 && (unsigned char)tb_s790[4] == 110 && (unsigned char)tb_s790[5] == 103 && tb_s790[6] == 0); }))) {
        f_emit__put(v_c, ((tb_o_Node *)tb_nn(v_n, 458))->q_value);
    } else if (({ const char *tb_s791 = v_k; ((unsigned char)tb_s791[0] == 98 && (unsigned char)tb_s791[1] == 111 && (unsigned char)tb_s791[2] == 111 && (unsigned char)tb_s791[3] == 108 && tb_s791[4] == 0); })) {
        if (({ const char *tb_s792 = ((tb_o_Node *)tb_nn(v_n, 460))->q_value; ((unsigned char)tb_s792[0] == 116 && (unsigned char)tb_s792[1] == 114 && (unsigned char)tb_s792[2] == 117 && (unsigned char)tb_s792[3] == 101 && tb_s792[4] == 0); })) {
            f_emit__put(v_c, "1");
        } else {
            f_emit__put(v_c, "0");
        }
    } else if (((({ const char *tb_s793 = v_k; ((unsigned char)tb_s793[0] == 110 && (unsigned char)tb_s793[1] == 97 && (unsigned char)tb_s793[2] == 109 && (unsigned char)tb_s793[3] == 101 && tb_s793[4] == 0); }) && ((((tb_o_Comp *)tb_nn(v_c, 464))->q_vec_names)->len > 0LL)) && (f_emit__vec_index(v_c, ((tb_o_Node *)tb_nn(v_n, 464))->q_name) >= 0LL))) {
        f_emit__put(v_c, (*(const char * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 465))->q_vec_repl, f_emit__vec_index(v_c, ((tb_o_Node *)tb_nn(v_n, 465))->q_name), sizeof(const char *), 465)));
    } else if (({ const char *tb_s794 = v_k; ((unsigned char)tb_s794[0] == 110 && (unsigned char)tb_s794[1] == 97 && (unsigned char)tb_s794[2] == 109 && (unsigned char)tb_s794[3] == 101 && tb_s794[4] == 0); })) {
        if (((tb_o_Node *)tb_nn(v_n, 467))->q_is_link) {
            f_emit__put(v_c, ({ const char *tb_c795_0 = "(*v_"; long long tb_c795_0n = sizeof("(*v_") - 1; const char *tb_c795_1 = ((tb_o_Node *)tb_nn(v_n, 468))->q_name; long long tb_c795_1n = (long long)strlen(tb_c795_1); const char *tb_c795_2 = ")"; long long tb_c795_2n = sizeof(")") - 1; char *tb_c795r = tb_mem(tb_c795_0n + tb_c795_1n + tb_c795_2n + 1), *tb_c795rw = tb_c795r; memcpy(tb_c795rw, tb_c795_0, tb_c795_0n); tb_c795rw += tb_c795_0n; memcpy(tb_c795rw, tb_c795_1, tb_c795_1n); tb_c795rw += tb_c795_1n; memcpy(tb_c795rw, tb_c795_2, tb_c795_2n); tb_c795rw += tb_c795_2n; *tb_c795rw = 0; (const char *)tb_c795r; }));
        } else if (f_emit__is_bld(v_c, v_n)) {
            f_emit__put(v_c, ({ const char *tb_c796_0 = "(tb_rl_"; long long tb_c796_0n = sizeof("(tb_rl_") - 1; const char *tb_c796_1 = ((tb_o_Node *)tb_nn(v_n, 470))->q_name; long long tb_c796_1n = (long long)strlen(tb_c796_1); const char *tb_c796_2 = " = -1, v_"; long long tb_c796_2n = sizeof(" = -1, v_") - 1; const char *tb_c796_3 = ((tb_o_Node *)tb_nn(v_n, 470))->q_name; long long tb_c796_3n = (long long)strlen(tb_c796_3); const char *tb_c796_4 = ")"; long long tb_c796_4n = sizeof(")") - 1; char *tb_c796r = tb_mem(tb_c796_0n + tb_c796_1n + tb_c796_2n + tb_c796_3n + tb_c796_4n + 1), *tb_c796rw = tb_c796r; memcpy(tb_c796rw, tb_c796_0, tb_c796_0n); tb_c796rw += tb_c796_0n; memcpy(tb_c796rw, tb_c796_1, tb_c796_1n); tb_c796rw += tb_c796_1n; memcpy(tb_c796rw, tb_c796_2, tb_c796_2n); tb_c796rw += tb_c796_2n; memcpy(tb_c796rw, tb_c796_3, tb_c796_3n); tb_c796rw += tb_c796_3n; memcpy(tb_c796rw, tb_c796_4, tb_c796_4n); tb_c796rw += tb_c796_4n; *tb_c796rw = 0; (const char *)tb_c796r; }));
        } else {
            f_emit__put(v_c, ({ const char *tb_c797_0 = "v_"; long long tb_c797_0n = sizeof("v_") - 1; const char *tb_c797_1 = ((tb_o_Node *)tb_nn(v_n, 472))->q_name; long long tb_c797_1n = (long long)strlen(tb_c797_1); char *tb_c797r = tb_mem(tb_c797_0n + tb_c797_1n + 1), *tb_c797rw = tb_c797r; memcpy(tb_c797rw, tb_c797_0, tb_c797_0n); tb_c797rw += tb_c797_0n; memcpy(tb_c797rw, tb_c797_1, tb_c797_1n); tb_c797rw += tb_c797_1n; *tb_c797rw = 0; (const char *)tb_c797r; }));
        }
    } else if (({ const char *tb_s798 = v_k; ((unsigned char)tb_s798[0] == 108 && (unsigned char)tb_s798[1] == 105 && (unsigned char)tb_s798[2] == 115 && (unsigned char)tb_s798[3] == 116 && tb_s798[4] == 0); })) {
        f_emit__emit_list(v_c, v_n);
    } else if (({ const char *tb_s799 = v_k; ((unsigned char)tb_s799[0] == 100 && (unsigned char)tb_s799[1] == 105 && (unsigned char)tb_s799[2] == 99 && (unsigned char)tb_s799[3] == 116 && tb_s799[4] == 0); })) {
        f_emit__emit_dict(v_c, v_n);
    } else if ((({ const char *tb_s800 = v_k; ((unsigned char)tb_s800[0] == 116 && (unsigned char)tb_s800[1] == 114 && (unsigned char)tb_s800[2] == 121 && tb_s800[3] == 0); }) || ({ const char *tb_s801 = v_k; ((unsigned char)tb_s801[0] == 101 && (unsigned char)tb_s801[1] == 108 && (unsigned char)tb_s801[2] == 115 && (unsigned char)tb_s801[3] == 101 && (unsigned char)tb_s801[4] == 95 && (unsigned char)tb_s801[5] == 101 && (unsigned char)tb_s801[6] == 114 && (unsigned char)tb_s801[7] == 114 && tb_s801[8] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c802_0 = "tb_v"; long long tb_c802_0n = sizeof("tb_v") - 1; char tb_c802_1d[24]; long long tb_c802_1n; const char *tb_c802_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 479))->q_temp_no, tb_c802_1d, &tb_c802_1n); char *tb_c802r = tb_mem(tb_c802_0n + tb_c802_1n + 1), *tb_c802rw = tb_c802r; memcpy(tb_c802rw, tb_c802_0, tb_c802_0n); tb_c802rw += tb_c802_0n; memcpy(tb_c802rw, tb_c802_1, tb_c802_1n); tb_c802rw += tb_c802_1n; *tb_c802rw = 0; (const char *)tb_c802r; }));
    } else if (({ const char *tb_s803 = v_k; ((unsigned char)tb_s803[0] == 99 && (unsigned char)tb_s803[1] == 97 && (unsigned char)tb_s803[2] == 108 && (unsigned char)tb_s803[3] == 108 && tb_s803[4] == 0); })) {
        f_emit__emit_call(v_c, v_n);
    } else if (({ const char *tb_s804 = v_k; ((unsigned char)tb_s804[0] == 109 && (unsigned char)tb_s804[1] == 101 && (unsigned char)tb_s804[2] == 116 && (unsigned char)tb_s804[3] == 104 && (unsigned char)tb_s804[4] == 111 && (unsigned char)tb_s804[5] == 100 && tb_s804[6] == 0); })) {
        f_emit__emit_method(v_c, v_n);
    } else if (({ const char *tb_s805 = v_k; ((unsigned char)tb_s805[0] == 98 && (unsigned char)tb_s805[1] == 105 && (unsigned char)tb_s805[2] == 110 && (unsigned char)tb_s805[3] == 111 && (unsigned char)tb_s805[4] == 112 && tb_s805[5] == 0); })) {
        f_emit__emit_binop(v_c, v_n);
    } else if (({ const char *tb_s806 = v_k; ((unsigned char)tb_s806[0] == 117 && (unsigned char)tb_s806[1] == 110 && (unsigned char)tb_s806[2] == 97 && (unsigned char)tb_s806[3] == 114 && (unsigned char)tb_s806[4] == 121 && tb_s806[5] == 0); })) {
        if (({ const char *tb_s807 = ((tb_o_Node *)tb_nn(v_n, 487))->q_value; ((unsigned char)tb_s807[0] == 110 && (unsigned char)tb_s807[1] == 111 && (unsigned char)tb_s807[2] == 116 && tb_s807[3] == 0); })) {
            f_emit__put(v_c, "(!");
        } else {
            f_emit__put(v_c, "(-");
        }
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 491))->q_kids, 0LL, sizeof(tb_o_Node *), 491)));
        f_emit__put(v_c, ")");
    } else if ((({ const char *tb_s808 = v_k; ((unsigned char)tb_s808[0] == 105 && (unsigned char)tb_s808[1] == 110 && (unsigned char)tb_s808[2] == 100 && (unsigned char)tb_s808[3] == 101 && (unsigned char)tb_s808[4] == 120 && tb_s808[5] == 0); }) && ({ const char *tb_s809 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 493))->q_kids, 0LL, sizeof(tb_o_Node *), 493)), 493))->q_ty); ((unsigned char)tb_s809[0] == 100 && (unsigned char)tb_s809[1] == 105 && (unsigned char)tb_s809[2] == 99 && (unsigned char)tb_s809[3] == 116 && tb_s809[4] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c810_0 = "(*("; long long tb_c810_0n = sizeof("(*(") - 1; const char *tb_c810_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 494))->q_ty); long long tb_c810_1n = (long long)strlen(tb_c810_1); const char *tb_c810_2 = " *)tb_dget("; long long tb_c810_2n = sizeof(" *)tb_dget(") - 1; char *tb_c810r = tb_mem(tb_c810_0n + tb_c810_1n + tb_c810_2n + 1), *tb_c810rw = tb_c810r; memcpy(tb_c810rw, tb_c810_0, tb_c810_0n); tb_c810rw += tb_c810_0n; memcpy(tb_c810rw, tb_c810_1, tb_c810_1n); tb_c810rw += tb_c810_1n; memcpy(tb_c810rw, tb_c810_2, tb_c810_2n); tb_c810rw += tb_c810_2n; *tb_c810rw = 0; (const char *)tb_c810r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 495))->q_kids, 0LL, sizeof(tb_o_Node *), 495)));
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 497))->q_kids, 0LL, sizeof(tb_o_Node *), 497)), 497))->q_ty, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 497))->q_kids, 1LL, sizeof(tb_o_Node *), 497)));
        f_emit__put(v_c, ({ const char *tb_c811_0 = ", "; long long tb_c811_0n = sizeof(", ") - 1; char tb_c811_1d[24]; long long tb_c811_1n; const char *tb_c811_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 498))->q_line, tb_c811_1d, &tb_c811_1n); const char *tb_c811_2 = "))"; long long tb_c811_2n = sizeof("))") - 1; char *tb_c811r = tb_mem(tb_c811_0n + tb_c811_1n + tb_c811_2n + 1), *tb_c811rw = tb_c811r; memcpy(tb_c811rw, tb_c811_0, tb_c811_0n); tb_c811rw += tb_c811_0n; memcpy(tb_c811rw, tb_c811_1, tb_c811_1n); tb_c811rw += tb_c811_1n; memcpy(tb_c811rw, tb_c811_2, tb_c811_2n); tb_c811rw += tb_c811_2n; *tb_c811rw = 0; (const char *)tb_c811r; }));
    } else if ((({ const char *tb_s812 = v_k; ((unsigned char)tb_s812[0] == 105 && (unsigned char)tb_s812[1] == 110 && (unsigned char)tb_s812[2] == 100 && (unsigned char)tb_s812[3] == 101 && (unsigned char)tb_s812[4] == 120 && tb_s812[5] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 499))->q_kids, 0LL, sizeof(tb_o_Node *), 499)), 499))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c813_0 = "tb_colat_"; long long tb_c813_0n = sizeof("tb_colat_") - 1; const char *tb_c813_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 500))->q_kids, 0LL, sizeof(tb_o_Node *), 500)), 500))->q_ty); long long tb_c813_1n = (long long)strlen(tb_c813_1); const char *tb_c813_2 = "("; long long tb_c813_2n = sizeof("(") - 1; char *tb_c813r = tb_mem(tb_c813_0n + tb_c813_1n + tb_c813_2n + 1), *tb_c813rw = tb_c813r; memcpy(tb_c813rw, tb_c813_0, tb_c813_0n); tb_c813rw += tb_c813_0n; memcpy(tb_c813rw, tb_c813_1, tb_c813_1n); tb_c813rw += tb_c813_1n; memcpy(tb_c813rw, tb_c813_2, tb_c813_2n); tb_c813rw += tb_c813_2n; *tb_c813rw = 0; (const char *)tb_c813r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 501))->q_kids, 0LL, sizeof(tb_o_Node *), 501)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 503))->q_kids, 1LL, sizeof(tb_o_Node *), 503)));
        f_emit__put(v_c, ({ const char *tb_c814_0 = ", "; long long tb_c814_0n = sizeof(", ") - 1; char tb_c814_1d[24]; long long tb_c814_1n; const char *tb_c814_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 504))->q_line, tb_c814_1d, &tb_c814_1n); const char *tb_c814_2 = ")"; long long tb_c814_2n = sizeof(")") - 1; char *tb_c814r = tb_mem(tb_c814_0n + tb_c814_1n + tb_c814_2n + 1), *tb_c814rw = tb_c814r; memcpy(tb_c814rw, tb_c814_0, tb_c814_0n); tb_c814rw += tb_c814_0n; memcpy(tb_c814rw, tb_c814_1, tb_c814_1n); tb_c814rw += tb_c814_1n; memcpy(tb_c814rw, tb_c814_2, tb_c814_2n); tb_c814rw += tb_c814_2n; *tb_c814rw = 0; (const char *)tb_c814r; }));
    } else if (({ const char *tb_s815 = v_k; ((unsigned char)tb_s815[0] == 105 && (unsigned char)tb_s815[1] == 110 && (unsigned char)tb_s815[2] == 100 && (unsigned char)tb_s815[3] == 101 && (unsigned char)tb_s815[4] == 120 && tb_s815[5] == 0); })) {
        if (({ const char *tb_s816 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 506))->q_kids, 0LL, sizeof(tb_o_Node *), 506)), 506))->q_ty); ((unsigned char)tb_s816[0] == 116 && (unsigned char)tb_s816[1] == 101 && (unsigned char)tb_s816[2] == 120 && (unsigned char)tb_s816[3] == 116 && tb_s816[4] == 0); })) {
            f_emit__put(v_c, "tb_char_at(");
            f_emit__emit_q(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 508))->q_kids, 0LL, sizeof(tb_o_Node *), 508)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 510))->q_kids, 1LL, sizeof(tb_o_Node *), 510)));
            if (f_emit__is_bld(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 511))->q_kids, 0LL, sizeof(tb_o_Node *), 511)))) {
                f_emit__put(v_c, ({ const char *tb_c817_0 = ", tb_rl_"; long long tb_c817_0n = sizeof(", tb_rl_") - 1; const char *tb_c817_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 512))->q_kids, 0LL, sizeof(tb_o_Node *), 512)), 512))->q_name; long long tb_c817_1n = (long long)strlen(tb_c817_1); char *tb_c817r = tb_mem(tb_c817_0n + tb_c817_1n + 1), *tb_c817rw = tb_c817r; memcpy(tb_c817rw, tb_c817_0, tb_c817_0n); tb_c817rw += tb_c817_0n; memcpy(tb_c817rw, tb_c817_1, tb_c817_1n); tb_c817rw += tb_c817_1n; *tb_c817rw = 0; (const char *)tb_c817r; }));
            } else {
                f_emit__put(v_c, ", -1");
            }
            f_emit__put(v_c, ({ const char *tb_c818_0 = ", "; long long tb_c818_0n = sizeof(", ") - 1; char tb_c818_1d[24]; long long tb_c818_1n; const char *tb_c818_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 515))->q_line, tb_c818_1d, &tb_c818_1n); const char *tb_c818_2 = ")"; long long tb_c818_2n = sizeof(")") - 1; char *tb_c818r = tb_mem(tb_c818_0n + tb_c818_1n + tb_c818_2n + 1), *tb_c818rw = tb_c818r; memcpy(tb_c818rw, tb_c818_0, tb_c818_0n); tb_c818rw += tb_c818_0n; memcpy(tb_c818rw, tb_c818_1, tb_c818_1n); tb_c818rw += tb_c818_1n; memcpy(tb_c818rw, tb_c818_2, tb_c818_2n); tb_c818rw += tb_c818_2n; *tb_c818rw = 0; (const char *)tb_c818r; }));
        } else if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 516))->q_kids, 0LL, sizeof(tb_o_Node *), 516)), 516))->q_ty)) {
            f_emit__put(v_c, "tb_bget(");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 518))->q_kids, 0LL, sizeof(tb_o_Node *), 518)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 520))->q_kids, 1LL, sizeof(tb_o_Node *), 520)));
            f_emit__put(v_c, ({ const char *tb_c819_0 = ", "; long long tb_c819_0n = sizeof(", ") - 1; char tb_c819_1d[24]; long long tb_c819_1n; const char *tb_c819_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 521))->q_line, tb_c819_1d, &tb_c819_1n); const char *tb_c819_2 = ")"; long long tb_c819_2n = sizeof(")") - 1; char *tb_c819r = tb_mem(tb_c819_0n + tb_c819_1n + tb_c819_2n + 1), *tb_c819rw = tb_c819r; memcpy(tb_c819rw, tb_c819_0, tb_c819_0n); tb_c819rw += tb_c819_0n; memcpy(tb_c819rw, tb_c819_1, tb_c819_1n); tb_c819rw += tb_c819_1n; memcpy(tb_c819rw, tb_c819_2, tb_c819_2n); tb_c819rw += tb_c819_2n; *tb_c819rw = 0; (const char *)tb_c819r; }));
        } else {
            const char * v_t = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 523))->q_ty);
            f_emit__put(v_c, ({ const char *tb_c820_0 = "(*("; long long tb_c820_0n = sizeof("(*(") - 1; const char *tb_c820_1 = v_t; long long tb_c820_1n = (long long)strlen(tb_c820_1); const char *tb_c820_2 = " *)tb_at("; long long tb_c820_2n = sizeof(" *)tb_at(") - 1; char *tb_c820r = tb_mem(tb_c820_0n + tb_c820_1n + tb_c820_2n + 1), *tb_c820rw = tb_c820r; memcpy(tb_c820rw, tb_c820_0, tb_c820_0n); tb_c820rw += tb_c820_0n; memcpy(tb_c820rw, tb_c820_1, tb_c820_1n); tb_c820rw += tb_c820_1n; memcpy(tb_c820rw, tb_c820_2, tb_c820_2n); tb_c820rw += tb_c820_2n; *tb_c820rw = 0; (const char *)tb_c820r; }));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 525))->q_kids, 0LL, sizeof(tb_o_Node *), 525)));
            f_emit__put(v_c, ", ");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 527))->q_kids, 1LL, sizeof(tb_o_Node *), 527)));
            f_emit__put(v_c, ({ const char *tb_c821_0 = ", sizeof("; long long tb_c821_0n = sizeof(", sizeof(") - 1; const char *tb_c821_1 = v_t; long long tb_c821_1n = (long long)strlen(tb_c821_1); const char *tb_c821_2 = "), "; long long tb_c821_2n = sizeof("), ") - 1; char tb_c821_3d[24]; long long tb_c821_3n; const char *tb_c821_3 = tb_digits(((tb_o_Node *)tb_nn(v_n, 528))->q_line, tb_c821_3d, &tb_c821_3n); const char *tb_c821_4 = "))"; long long tb_c821_4n = sizeof("))") - 1; char *tb_c821r = tb_mem(tb_c821_0n + tb_c821_1n + tb_c821_2n + tb_c821_3n + tb_c821_4n + 1), *tb_c821rw = tb_c821r; memcpy(tb_c821rw, tb_c821_0, tb_c821_0n); tb_c821rw += tb_c821_0n; memcpy(tb_c821rw, tb_c821_1, tb_c821_1n); tb_c821rw += tb_c821_1n; memcpy(tb_c821rw, tb_c821_2, tb_c821_2n); tb_c821rw += tb_c821_2n; memcpy(tb_c821rw, tb_c821_3, tb_c821_3n); tb_c821rw += tb_c821_3n; memcpy(tb_c821rw, tb_c821_4, tb_c821_4n); tb_c821rw += tb_c821_4n; *tb_c821rw = 0; (const char *)tb_c821r; }));
        }
    } else if ((({ const char *tb_s822 = v_k; ((unsigned char)tb_s822[0] == 102 && (unsigned char)tb_s822[1] == 105 && (unsigned char)tb_s822[2] == 101 && (unsigned char)tb_s822[3] == 108 && (unsigned char)tb_s822[4] == 100 && tb_s822[5] == 0); }) && ({ const char *tb_s823 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 529))->q_kids, 0LL, sizeof(tb_o_Node *), 529)), 529))->q_ty); ((unsigned char)tb_s823[0] == 116 && (unsigned char)tb_s823[1] == 97 && (unsigned char)tb_s823[2] == 115 && (unsigned char)tb_s823[3] == 107 && tb_s823[4] == 0); }))) {
        long long v_rt = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 530))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 530))->q_kids, 0LL, sizeof(tb_o_Node *), 530)), 530))->q_ty, sizeof(tb_o_Type *), 530)), 530))->q_elem;
        if ((v_rt == 0LL)) {
            f_emit__put(v_c, "((void)tb_wait(");
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 533))->q_kids, 0LL, sizeof(tb_o_Node *), 533)));
            f_emit__put(v_c, "))");
        } else {
            f_emit__put(v_c, ({ const char *tb_c824_0 = "(*("; long long tb_c824_0n = sizeof("(*(") - 1; const char *tb_c824_1 = f_emit__c_type(v_c, v_rt); long long tb_c824_1n = (long long)strlen(tb_c824_1); const char *tb_c824_2 = " *)tb_wait("; long long tb_c824_2n = sizeof(" *)tb_wait(") - 1; char *tb_c824r = tb_mem(tb_c824_0n + tb_c824_1n + tb_c824_2n + 1), *tb_c824rw = tb_c824r; memcpy(tb_c824rw, tb_c824_0, tb_c824_0n); tb_c824rw += tb_c824_0n; memcpy(tb_c824rw, tb_c824_1, tb_c824_1n); tb_c824rw += tb_c824_1n; memcpy(tb_c824rw, tb_c824_2, tb_c824_2n); tb_c824rw += tb_c824_2n; *tb_c824rw = 0; (const char *)tb_c824r; }));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 537))->q_kids, 0LL, sizeof(tb_o_Node *), 537)));
            f_emit__put(v_c, "))");
        }
    } else if ((strcmp(v_k, "background") == 0)) {
        f_emit__emit_background(v_c, v_n);
    } else if ((({ const char *tb_s825 = v_k; ((unsigned char)tb_s825[0] == 102 && (unsigned char)tb_s825[1] == 105 && (unsigned char)tb_s825[2] == 101 && (unsigned char)tb_s825[3] == 108 && (unsigned char)tb_s825[4] == 100 && tb_s825[5] == 0); }) && f_check__is_cols_obj(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 541))->q_kids, 0LL, sizeof(tb_o_Node *), 541)), 541))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c826_0 = "(*tb_colf_"; long long tb_c826_0n = sizeof("(*tb_colf_") - 1; const char *tb_c826_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 543))->q_kids, 0LL, sizeof(tb_o_Node *), 543)), 543))->q_ty); long long tb_c826_1n = (long long)strlen(tb_c826_1); const char *tb_c826_2 = "_"; long long tb_c826_2n = sizeof("_") - 1; const char *tb_c826_3 = ((tb_o_Node *)tb_nn(v_n, 543))->q_name; long long tb_c826_3n = (long long)strlen(tb_c826_3); const char *tb_c826_4 = "("; long long tb_c826_4n = sizeof("(") - 1; char *tb_c826r = tb_mem(tb_c826_0n + tb_c826_1n + tb_c826_2n + tb_c826_3n + tb_c826_4n + 1), *tb_c826rw = tb_c826r; memcpy(tb_c826rw, tb_c826_0, tb_c826_0n); tb_c826rw += tb_c826_0n; memcpy(tb_c826rw, tb_c826_1, tb_c826_1n); tb_c826rw += tb_c826_1n; memcpy(tb_c826rw, tb_c826_2, tb_c826_2n); tb_c826rw += tb_c826_2n; memcpy(tb_c826rw, tb_c826_3, tb_c826_3n); tb_c826rw += tb_c826_3n; memcpy(tb_c826rw, tb_c826_4, tb_c826_4n); tb_c826rw += tb_c826_4n; *tb_c826rw = 0; (const char *)tb_c826r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 544))->q_kids, 0LL, sizeof(tb_o_Node *), 544)));
        f_emit__put(v_c, ({ const char *tb_c827_0 = ", "; long long tb_c827_0n = sizeof(", ") - 1; char tb_c827_1d[24]; long long tb_c827_1n; const char *tb_c827_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 545))->q_line, tb_c827_1d, &tb_c827_1n); const char *tb_c827_2 = "))"; long long tb_c827_2n = sizeof("))") - 1; char *tb_c827r = tb_mem(tb_c827_0n + tb_c827_1n + tb_c827_2n + 1), *tb_c827rw = tb_c827r; memcpy(tb_c827rw, tb_c827_0, tb_c827_0n); tb_c827rw += tb_c827_0n; memcpy(tb_c827rw, tb_c827_1, tb_c827_1n); tb_c827rw += tb_c827_1n; memcpy(tb_c827rw, tb_c827_2, tb_c827_2n); tb_c827rw += tb_c827_2n; *tb_c827rw = 0; (const char *)tb_c827r; }));
    } else if (({ const char *tb_s828 = v_k; ((unsigned char)tb_s828[0] == 102 && (unsigned char)tb_s828[1] == 105 && (unsigned char)tb_s828[2] == 101 && (unsigned char)tb_s828[3] == 108 && (unsigned char)tb_s828[4] == 100 && tb_s828[5] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c829_0 = "(("; long long tb_c829_0n = sizeof("((") - 1; const char *tb_c829_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 547))->q_kids, 0LL, sizeof(tb_o_Node *), 547)), 547))->q_ty); long long tb_c829_1n = (long long)strlen(tb_c829_1); const char *tb_c829_2 = ")tb_nn("; long long tb_c829_2n = sizeof(")tb_nn(") - 1; char *tb_c829r = tb_mem(tb_c829_0n + tb_c829_1n + tb_c829_2n + 1), *tb_c829rw = tb_c829r; memcpy(tb_c829rw, tb_c829_0, tb_c829_0n); tb_c829rw += tb_c829_0n; memcpy(tb_c829rw, tb_c829_1, tb_c829_1n); tb_c829rw += tb_c829_1n; memcpy(tb_c829rw, tb_c829_2, tb_c829_2n); tb_c829rw += tb_c829_2n; *tb_c829rw = 0; (const char *)tb_c829r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 548))->q_kids, 0LL, sizeof(tb_o_Node *), 548)));
        f_emit__put(v_c, ({ const char *tb_c830_0 = ", "; long long tb_c830_0n = sizeof(", ") - 1; char tb_c830_1d[24]; long long tb_c830_1n; const char *tb_c830_1 = tb_digits(((tb_o_Node *)tb_nn(v_n, 549))->q_line, tb_c830_1d, &tb_c830_1n); const char *tb_c830_2 = "))"; long long tb_c830_2n = sizeof("))") - 1; const char *tb_c830_3 = f_emit__field_path(((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 549))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 549))->q_kids, 0LL, sizeof(tb_o_Node *), 549)), 549))->q_ty, sizeof(tb_o_Type *), 549)), 549))->q_obj_node, ((tb_o_Node *)tb_nn(v_n, 549))->q_name); long long tb_c830_3n = (long long)strlen(tb_c830_3); char *tb_c830r = tb_mem(tb_c830_0n + tb_c830_1n + tb_c830_2n + tb_c830_3n + 1), *tb_c830rw = tb_c830r; memcpy(tb_c830rw, tb_c830_0, tb_c830_0n); tb_c830rw += tb_c830_0n; memcpy(tb_c830rw, tb_c830_1, tb_c830_1n); tb_c830rw += tb_c830_1n; memcpy(tb_c830rw, tb_c830_2, tb_c830_2n); tb_c830rw += tb_c830_2n; memcpy(tb_c830rw, tb_c830_3, tb_c830_3n); tb_c830rw += tb_c830_3n; *tb_c830rw = 0; (const char *)tb_c830r; }));
    }
}

static void f_emit__emit_bail(tb_o_Comp * v_c) {
    tb_o_Node * v_f = ((tb_o_Comp *)tb_nn(v_c, 555))->q_cur_func;
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 556))->q_kids, 2LL, sizeof(tb_o_Node *), 556));
    if (({ const char *tb_s831 = ((tb_o_Node *)tb_nn(v_f, 557))->q_name; ((unsigned char)tb_s831[0] == 109 && (unsigned char)tb_s831[1] == 97 && (unsigned char)tb_s831[2] == 105 && (unsigned char)tb_s831[3] == 110 && tb_s831[4] == 0); })) {
        f_emit__put(v_c, "tb_die(tb_err);");
    } else if (((((tb_o_Node *)tb_nn(v_outputs, 559))->q_kids)->len == 1LL)) {
        f_emit__put(v_c, ({ const char *tb_c832_0 = "return v_"; long long tb_c832_0n = sizeof("return v_") - 1; const char *tb_c832_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 560))->q_kids, 0LL, sizeof(tb_o_Node *), 560)), 560))->q_name; long long tb_c832_1n = (long long)strlen(tb_c832_1); const char *tb_c832_2 = ";"; long long tb_c832_2n = sizeof(";") - 1; char *tb_c832r = tb_mem(tb_c832_0n + tb_c832_1n + tb_c832_2n + 1), *tb_c832rw = tb_c832r; memcpy(tb_c832rw, tb_c832_0, tb_c832_0n); tb_c832rw += tb_c832_0n; memcpy(tb_c832rw, tb_c832_1, tb_c832_1n); tb_c832rw += tb_c832_1n; memcpy(tb_c832rw, tb_c832_2, tb_c832_2n); tb_c832rw += tb_c832_2n; *tb_c832rw = 0; (const char *)tb_c832r; }));
    } else if (((((tb_o_Node *)tb_nn(v_outputs, 561))->q_kids)->len > 1LL)) {
        f_emit__put(v_c, ({ const char *tb_c833_0 = "return ("; long long tb_c833_0n = sizeof("return (") - 1; const char *tb_c833_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 562))->q_ty); long long tb_c833_1n = (long long)strlen(tb_c833_1); const char *tb_c833_2 = "){"; long long tb_c833_2n = sizeof("){") - 1; char *tb_c833r = tb_mem(tb_c833_0n + tb_c833_1n + tb_c833_2n + 1), *tb_c833rw = tb_c833r; memcpy(tb_c833rw, tb_c833_0, tb_c833_0n); tb_c833rw += tb_c833_0n; memcpy(tb_c833rw, tb_c833_1, tb_c833_1n); tb_c833rw += tb_c833_1n; memcpy(tb_c833rw, tb_c833_2, tb_c833_2n); tb_c833rw += tb_c833_2n; *tb_c833rw = 0; (const char *)tb_c833r; }));
        {
            long long tb_end834 = ((((tb_o_Node *)tb_nn(v_outputs, 563))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end834; v_i++) {
                if ((v_i > 0LL)) {
                    f_emit__put(v_c, ", ");
                }
                f_emit__put(v_c, ({ const char *tb_c835_0 = "v_"; long long tb_c835_0n = sizeof("v_") - 1; const char *tb_c835_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 566))->q_kids, v_i, sizeof(tb_o_Node *), 566)), 566))->q_name; long long tb_c835_1n = (long long)strlen(tb_c835_1); char *tb_c835r = tb_mem(tb_c835_0n + tb_c835_1n + 1), *tb_c835rw = tb_c835r; memcpy(tb_c835rw, tb_c835_0, tb_c835_0n); tb_c835rw += tb_c835_0n; memcpy(tb_c835rw, tb_c835_1, tb_c835_1n); tb_c835rw += tb_c835_1n; *tb_c835rw = 0; (const char *)tb_c835r; }));
            }
        }
        f_emit__put(v_c, "};");
    } else if (({ const char *tb_s836 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 568))->q_ty); ((unsigned char)tb_s836[0] == 110 && (unsigned char)tb_s836[1] == 111 && (unsigned char)tb_s836[2] == 110 && (unsigned char)tb_s836[3] == 101 && tb_s836[4] == 0); })) {
        f_emit__put(v_c, "return;");
    } else {
        f_emit__put(v_c, "return ");
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_f, 572))->q_ty);
        f_emit__put(v_c, ";");
    }
}

static void f_emit__emit_pre(tb_o_Comp * v_c, tb_o_Node * v_v, long long v_depth) {
    if ((({ const char *tb_s837 = ((tb_o_Node *)tb_nn(v_v, 577))->q_kind; !((unsigned char)tb_s837[0] == 116 && (unsigned char)tb_s837[1] == 114 && (unsigned char)tb_s837[2] == 121 && tb_s837[3] == 0); }) && ({ const char *tb_s838 = ((tb_o_Node *)tb_nn(v_v, 577))->q_kind; !((unsigned char)tb_s838[0] == 101 && (unsigned char)tb_s838[1] == 108 && (unsigned char)tb_s838[2] == 115 && (unsigned char)tb_s838[3] == 101 && (unsigned char)tb_s838[4] == 95 && (unsigned char)tb_s838[5] == 101 && (unsigned char)tb_s838[6] == 114 && (unsigned char)tb_s838[7] == 114 && tb_s838[8] == 0); }))) {
        return;
    }
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 579))->q_kids, 0LL, sizeof(tb_o_Node *), 579));
    { long long tb_v839 = 1LL; long long *tb_p839 = &(((tb_o_Comp *)tb_nn(v_c, 580))->q_label); *tb_p839 += tb_v839; }
    { long long tb_v840 = ((tb_o_Comp *)tb_nn(v_c, 581))->q_label; long long *tb_p840 = &(((tb_o_Node *)tb_nn(v_v, 581))->q_temp_no); *tb_p840 = tb_v840; }
    f_emit__indent(v_c, v_depth);
    if (({ const char *tb_s841 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_call, 583))->q_ty); !((unsigned char)tb_s841[0] == 110 && (unsigned char)tb_s841[1] == 111 && (unsigned char)tb_s841[2] == 110 && (unsigned char)tb_s841[3] == 101 && tb_s841[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c842_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_call, 584))->q_ty); long long tb_c842_0n = (long long)strlen(tb_c842_0); const char *tb_c842_1 = " tb_v"; long long tb_c842_1n = sizeof(" tb_v") - 1; char tb_c842_2d[24]; long long tb_c842_2n; const char *tb_c842_2 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 584))->q_label, tb_c842_2d, &tb_c842_2n); const char *tb_c842_3 = " = "; long long tb_c842_3n = sizeof(" = ") - 1; char *tb_c842r = tb_mem(tb_c842_0n + tb_c842_1n + tb_c842_2n + tb_c842_3n + 1), *tb_c842rw = tb_c842r; memcpy(tb_c842rw, tb_c842_0, tb_c842_0n); tb_c842rw += tb_c842_0n; memcpy(tb_c842rw, tb_c842_1, tb_c842_1n); tb_c842rw += tb_c842_1n; memcpy(tb_c842rw, tb_c842_2, tb_c842_2n); tb_c842rw += tb_c842_2n; memcpy(tb_c842rw, tb_c842_3, tb_c842_3n); tb_c842rw += tb_c842_3n; *tb_c842rw = 0; (const char *)tb_c842r; }));
    }
    f_emit__emit_expr(v_c, v_call);
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, v_depth);
    if (({ const char *tb_s843 = ((tb_o_Node *)tb_nn(v_v, 588))->q_kind; ((unsigned char)tb_s843[0] == 116 && (unsigned char)tb_s843[1] == 114 && (unsigned char)tb_s843[2] == 121 && tb_s843[3] == 0); })) {
        f_emit__put(v_c, "if (TB_RARE(tb_err != 0)) { ");
        f_emit__emit_bail(v_c);
        f_emit__put(v_c, " }\n");
        return;
    }
    f_emit__put(v_c, "if (tb_err != 0) {\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c844_0 = "const char * const v_"; long long tb_c844_0n = sizeof("const char * const v_") - 1; const char *tb_c844_1 = ((tb_o_Node *)tb_nn(v_v, 595))->q_name; long long tb_c844_1n = (long long)strlen(tb_c844_1); const char *tb_c844_2 = " = tb_err;\n"; long long tb_c844_2n = sizeof(" = tb_err;\n") - 1; char *tb_c844r = tb_mem(tb_c844_0n + tb_c844_1n + tb_c844_2n + 1), *tb_c844rw = tb_c844r; memcpy(tb_c844rw, tb_c844_0, tb_c844_0n); tb_c844rw += tb_c844_0n; memcpy(tb_c844rw, tb_c844_1, tb_c844_1n); tb_c844rw += tb_c844_1n; memcpy(tb_c844rw, tb_c844_2, tb_c844_2n); tb_c844rw += tb_c844_2n; *tb_c844rw = 0; (const char *)tb_c844r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "tb_err = 0;\n");
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 598))->q_kids, 1LL, sizeof(tb_o_Node *), 598)), (v_depth + 1LL));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_target(tb_o_Comp * v_c, tb_o_Node * v_t) {
    if ((({ const char *tb_s845 = ((tb_o_Node *)tb_nn(v_t, 604))->q_kind; ((unsigned char)tb_s845[0] == 105 && (unsigned char)tb_s845[1] == 110 && (unsigned char)tb_s845[2] == 100 && (unsigned char)tb_s845[3] == 101 && (unsigned char)tb_s845[4] == 120 && tb_s845[5] == 0); }) && ({ const char *tb_s846 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 604))->q_kids, 0LL, sizeof(tb_o_Node *), 604)), 604))->q_ty); ((unsigned char)tb_s846[0] == 100 && (unsigned char)tb_s846[1] == 105 && (unsigned char)tb_s846[2] == 99 && (unsigned char)tb_s846[3] == 116 && tb_s846[4] == 0); }))) {
        long long v_d = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 605))->q_kids, 0LL, sizeof(tb_o_Node *), 605)), 605))->q_ty;
        long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 606))->q_types, v_d, sizeof(tb_o_Type *), 606)), 606))->q_elem;
        const char * v_vt = f_emit__c_type(v_c, v_elem);
        f_emit__put(v_c, ({ const char *tb_c847_0 = "(*("; long long tb_c847_0n = sizeof("(*(") - 1; const char *tb_c847_1 = v_vt; long long tb_c847_1n = (long long)strlen(tb_c847_1); const char *tb_c847_2 = " *)tb_dput("; long long tb_c847_2n = sizeof(" *)tb_dput(") - 1; char *tb_c847r = tb_mem(tb_c847_0n + tb_c847_1n + tb_c847_2n + 1), *tb_c847rw = tb_c847r; memcpy(tb_c847rw, tb_c847_0, tb_c847_0n); tb_c847rw += tb_c847_0n; memcpy(tb_c847rw, tb_c847_1, tb_c847_1n); tb_c847rw += tb_c847_1n; memcpy(tb_c847rw, tb_c847_2, tb_c847_2n); tb_c847rw += tb_c847_2n; *tb_c847rw = 0; (const char *)tb_c847r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 609))->q_kids, 0LL, sizeof(tb_o_Node *), 609)));
        f_emit__put(v_c, ", ");
        f_emit__emit_key(v_c, v_d, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_t, 611))->q_kids, 1LL, sizeof(tb_o_Node *), 611)));
        if (f_emit__is_simple(v_c, v_elem)) {
            f_emit__put(v_c, ({ const char *tb_c848_0 = ", &("; long long tb_c848_0n = sizeof(", &(") - 1; const char *tb_c848_1 = v_vt; long long tb_c848_1n = (long long)strlen(tb_c848_1); const char *tb_c848_2 = "){"; long long tb_c848_2n = sizeof("){") - 1; char *tb_c848r = tb_mem(tb_c848_0n + tb_c848_1n + tb_c848_2n + 1), *tb_c848rw = tb_c848r; memcpy(tb_c848rw, tb_c848_0, tb_c848_0n); tb_c848rw += tb_c848_0n; memcpy(tb_c848rw, tb_c848_1, tb_c848_1n); tb_c848rw += tb_c848_1n; memcpy(tb_c848rw, tb_c848_2, tb_c848_2n); tb_c848rw += tb_c848_2n; *tb_c848rw = 0; (const char *)tb_c848r; }));
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
    _Bool v_bld = (((!((tb_o_Node *)tb_nn(v_n, 625))->q_is_link) && ({ const char *tb_s849 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 625))->q_ty); ((unsigned char)tb_s849[0] == 116 && (unsigned char)tb_s849[1] == 101 && (unsigned char)tb_s849[2] == 120 && (unsigned char)tb_s849[3] == 116 && tb_s849[4] == 0); })) && f_emit__bld_has(((tb_o_Comp *)tb_nn(v_c, 625))->q_builders, ((tb_o_Node *)tb_nn(v_n, 625))->q_name));
    if ((((v_bld && (!((tb_o_Node *)tb_nn(v_n, 626))->q_creates)) && ((((tb_o_Node *)tb_nn(v_n, 626))->q_kids)->len == 1LL)) && f_emit__app_form(v_c, ((tb_o_Node *)tb_nn(v_n, 626))->q_name, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 626))->q_kids, 0LL, sizeof(tb_o_Node *), 626))))) {
        {
            tb_list * tb_l850 = f_emit__app_parts(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 628))->q_kids, 0LL, sizeof(tb_o_Node *), 628)));
            for (long long tb_i850 = 0; tb_i850 < tb_l850->len; tb_i850++) {
                tb_o_Node * const v_part = ((tb_o_Node * *)tb_l850->data)[tb_i850];
                f_emit__emit_app(v_c, ((tb_o_Node *)tb_nn(v_n, 629))->q_name, v_part);
                f_emit__put(v_c, " ");
            }
        }
        f_emit__put(v_c, "\n");
        return;
    }
    if (((tb_o_Node *)tb_nn(v_n, 633))->q_creates) {
        f_emit__put(v_c, ({ const char *tb_c851_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 634))->q_ty); long long tb_c851_0n = (long long)strlen(tb_c851_0); const char *tb_c851_1 = " "; long long tb_c851_1n = sizeof(" ") - 1; char *tb_c851r = tb_mem(tb_c851_0n + tb_c851_1n + 1), *tb_c851rw = tb_c851r; memcpy(tb_c851rw, tb_c851_0, tb_c851_0n); tb_c851rw += tb_c851_0n; memcpy(tb_c851rw, tb_c851_1, tb_c851_1n); tb_c851rw += tb_c851_1n; *tb_c851rw = 0; (const char *)tb_c851r; }));
        if (((tb_o_Node *)tb_nn(v_n, 635))->q_is_const) {
            f_emit__put(v_c, "const ");
        }
        f_emit__put(v_c, ({ const char *tb_c852_0 = "v_"; long long tb_c852_0n = sizeof("v_") - 1; const char *tb_c852_1 = ((tb_o_Node *)tb_nn(v_n, 637))->q_name; long long tb_c852_1n = (long long)strlen(tb_c852_1); const char *tb_c852_2 = " = "; long long tb_c852_2n = sizeof(" = ") - 1; char *tb_c852r = tb_mem(tb_c852_0n + tb_c852_1n + tb_c852_2n + 1), *tb_c852rw = tb_c852r; memcpy(tb_c852rw, tb_c852_0, tb_c852_0n); tb_c852rw += tb_c852_0n; memcpy(tb_c852rw, tb_c852_1, tb_c852_1n); tb_c852rw += tb_c852_1n; memcpy(tb_c852rw, tb_c852_2, tb_c852_2n); tb_c852rw += tb_c852_2n; *tb_c852rw = 0; (const char *)tb_c852r; }));
    } else if (((tb_o_Node *)tb_nn(v_n, 638))->q_is_link) {
        f_emit__put(v_c, ({ const char *tb_c853_0 = "(*v_"; long long tb_c853_0n = sizeof("(*v_") - 1; const char *tb_c853_1 = ((tb_o_Node *)tb_nn(v_n, 639))->q_name; long long tb_c853_1n = (long long)strlen(tb_c853_1); const char *tb_c853_2 = ") = "; long long tb_c853_2n = sizeof(") = ") - 1; char *tb_c853r = tb_mem(tb_c853_0n + tb_c853_1n + tb_c853_2n + 1), *tb_c853rw = tb_c853r; memcpy(tb_c853rw, tb_c853_0, tb_c853_0n); tb_c853rw += tb_c853_0n; memcpy(tb_c853rw, tb_c853_1, tb_c853_1n); tb_c853rw += tb_c853_1n; memcpy(tb_c853rw, tb_c853_2, tb_c853_2n); tb_c853rw += tb_c853_2n; *tb_c853rw = 0; (const char *)tb_c853r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c854_0 = "v_"; long long tb_c854_0n = sizeof("v_") - 1; const char *tb_c854_1 = ((tb_o_Node *)tb_nn(v_n, 641))->q_name; long long tb_c854_1n = (long long)strlen(tb_c854_1); const char *tb_c854_2 = " = "; long long tb_c854_2n = sizeof(" = ") - 1; char *tb_c854r = tb_mem(tb_c854_0n + tb_c854_1n + tb_c854_2n + 1), *tb_c854rw = tb_c854r; memcpy(tb_c854rw, tb_c854_0, tb_c854_0n); tb_c854rw += tb_c854_0n; memcpy(tb_c854rw, tb_c854_1, tb_c854_1n); tb_c854rw += tb_c854_1n; memcpy(tb_c854rw, tb_c854_2, tb_c854_2n); tb_c854rw += tb_c854_2n; *tb_c854rw = 0; (const char *)tb_c854r; }));
    }
    if (((((tb_o_Node *)tb_nn(v_n, 642))->q_kids)->len == 1LL)) {
        f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 643))->q_kids, 0LL, sizeof(tb_o_Node *), 643)), ((tb_o_Node *)tb_nn(v_n, 643))->q_ty);
    } else {
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_n, 645))->q_ty);
    }
    f_emit__put(v_c, ";");
    if (v_bld) {
        f_emit__put(v_c, ({ const char *tb_c855_0 = " tb_rl_"; long long tb_c855_0n = sizeof(" tb_rl_") - 1; const char *tb_c855_1 = ((tb_o_Node *)tb_nn(v_n, 649))->q_name; long long tb_c855_1n = (long long)strlen(tb_c855_1); const char *tb_c855_2 = " = -1;"; long long tb_c855_2n = sizeof(" = -1;") - 1; char *tb_c855r = tb_mem(tb_c855_0n + tb_c855_1n + tb_c855_2n + 1), *tb_c855rw = tb_c855r; memcpy(tb_c855rw, tb_c855_0, tb_c855_0n); tb_c855rw += tb_c855_0n; memcpy(tb_c855rw, tb_c855_1, tb_c855_1n); tb_c855rw += tb_c855_1n; memcpy(tb_c855rw, tb_c855_2, tb_c855_2n); tb_c855rw += tb_c855_2n; *tb_c855rw = 0; (const char *)tb_c855r; }));
    }
    f_emit__put(v_c, "\n");
}

static void f_emit__emit_set(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 654))->q_kids, 0LL, sizeof(tb_o_Node *), 654));
    tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 655))->q_kids, 1LL, sizeof(tb_o_Node *), 655));
    const char * v_op = ((tb_o_Node *)tb_nn(v_n, 656))->q_value;
    const char * v_t = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 657))->q_ty);
    f_emit__emit_pre(v_c, v_value, v_depth);
    { long long tb_v856 = 1LL; long long *tb_p856 = &(((tb_o_Comp *)tb_nn(v_c, 659))->q_label); *tb_p856 += tb_v856; }
    const char * v_id = ({ const char *tb_c857_0 = ""; long long tb_c857_0n = sizeof("") - 1; char tb_c857_1d[24]; long long tb_c857_1n; const char *tb_c857_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 660))->q_label, tb_c857_1d, &tb_c857_1n); char *tb_c857r = tb_mem(tb_c857_0n + tb_c857_1n + 1), *tb_c857rw = tb_c857r; memcpy(tb_c857rw, tb_c857_0, tb_c857_0n); tb_c857rw += tb_c857_0n; memcpy(tb_c857rw, tb_c857_1, tb_c857_1n); tb_c857rw += tb_c857_1n; *tb_c857rw = 0; (const char *)tb_c857r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{ ");
    if ((({ const char *tb_s858 = ((tb_o_Node *)tb_nn(v_target, 663))->q_kind; ((unsigned char)tb_s858[0] == 105 && (unsigned char)tb_s858[1] == 110 && (unsigned char)tb_s858[2] == 100 && (unsigned char)tb_s858[3] == 101 && (unsigned char)tb_s858[4] == 120 && tb_s858[5] == 0); }) && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 663))->q_kids, 0LL, sizeof(tb_o_Node *), 663)), 663))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c859_0 = v_t; long long tb_c859_0n = (long long)strlen(tb_c859_0); const char *tb_c859_1 = " tb_v"; long long tb_c859_1n = sizeof(" tb_v") - 1; const char *tb_c859_2 = v_id; long long tb_c859_2n = (long long)strlen(tb_c859_2); const char *tb_c859_3 = " = "; long long tb_c859_3n = sizeof(" = ") - 1; char *tb_c859r = tb_mem(tb_c859_0n + tb_c859_1n + tb_c859_2n + tb_c859_3n + 1), *tb_c859rw = tb_c859r; memcpy(tb_c859rw, tb_c859_0, tb_c859_0n); tb_c859rw += tb_c859_0n; memcpy(tb_c859rw, tb_c859_1, tb_c859_1n); tb_c859rw += tb_c859_1n; memcpy(tb_c859rw, tb_c859_2, tb_c859_2n); tb_c859rw += tb_c859_2n; memcpy(tb_c859rw, tb_c859_3, tb_c859_3n); tb_c859rw += tb_c859_3n; *tb_c859rw = 0; (const char *)tb_c859r; }));
        f_emit__emit_expr(v_c, v_value);
        f_emit__put(v_c, ({ const char *tb_c860_0 = "; tb_colset_"; long long tb_c860_0n = sizeof("; tb_colset_") - 1; const char *tb_c860_1 = f_emit__cols_name(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 667))->q_kids, 0LL, sizeof(tb_o_Node *), 667)), 667))->q_ty); long long tb_c860_1n = (long long)strlen(tb_c860_1); const char *tb_c860_2 = "("; long long tb_c860_2n = sizeof("(") - 1; char *tb_c860r = tb_mem(tb_c860_0n + tb_c860_1n + tb_c860_2n + 1), *tb_c860rw = tb_c860r; memcpy(tb_c860rw, tb_c860_0, tb_c860_0n); tb_c860rw += tb_c860_0n; memcpy(tb_c860rw, tb_c860_1, tb_c860_1n); tb_c860rw += tb_c860_1n; memcpy(tb_c860rw, tb_c860_2, tb_c860_2n); tb_c860rw += tb_c860_2n; *tb_c860rw = 0; (const char *)tb_c860r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 668))->q_kids, 0LL, sizeof(tb_o_Node *), 668)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 670))->q_kids, 1LL, sizeof(tb_o_Node *), 670)));
        f_emit__put(v_c, ({ const char *tb_c861_0 = ", tb_v"; long long tb_c861_0n = sizeof(", tb_v") - 1; const char *tb_c861_1 = v_id; long long tb_c861_1n = (long long)strlen(tb_c861_1); const char *tb_c861_2 = ", "; long long tb_c861_2n = sizeof(", ") - 1; char tb_c861_3d[24]; long long tb_c861_3n; const char *tb_c861_3 = tb_digits(((tb_o_Node *)tb_nn(v_n, 671))->q_line, tb_c861_3d, &tb_c861_3n); const char *tb_c861_4 = "); }\n"; long long tb_c861_4n = sizeof("); }\n") - 1; char *tb_c861r = tb_mem(tb_c861_0n + tb_c861_1n + tb_c861_2n + tb_c861_3n + tb_c861_4n + 1), *tb_c861rw = tb_c861r; memcpy(tb_c861rw, tb_c861_0, tb_c861_0n); tb_c861rw += tb_c861_0n; memcpy(tb_c861rw, tb_c861_1, tb_c861_1n); tb_c861rw += tb_c861_1n; memcpy(tb_c861rw, tb_c861_2, tb_c861_2n); tb_c861rw += tb_c861_2n; memcpy(tb_c861rw, tb_c861_3, tb_c861_3n); tb_c861rw += tb_c861_3n; memcpy(tb_c861rw, tb_c861_4, tb_c861_4n); tb_c861rw += tb_c861_4n; *tb_c861rw = 0; (const char *)tb_c861r; }));
        return;
    }
    if (((({ const char *tb_s862 = ((tb_o_Node *)tb_nn(v_target, 673))->q_kind; ((unsigned char)tb_s862[0] == 105 && (unsigned char)tb_s862[1] == 110 && (unsigned char)tb_s862[2] == 100 && (unsigned char)tb_s862[3] == 101 && (unsigned char)tb_s862[4] == 120 && tb_s862[5] == 0); }) && f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 673))->q_kids, 0LL, sizeof(tb_o_Node *), 673)), 673))->q_ty)) && ({ const char *tb_s863 = v_op; ((unsigned char)tb_s863[0] == 61 && tb_s863[1] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c864_0 = "_Bool tb_v"; long long tb_c864_0n = sizeof("_Bool tb_v") - 1; const char *tb_c864_1 = v_id; long long tb_c864_1n = (long long)strlen(tb_c864_1); const char *tb_c864_2 = " = "; long long tb_c864_2n = sizeof(" = ") - 1; char *tb_c864r = tb_mem(tb_c864_0n + tb_c864_1n + tb_c864_2n + 1), *tb_c864rw = tb_c864r; memcpy(tb_c864rw, tb_c864_0, tb_c864_0n); tb_c864rw += tb_c864_0n; memcpy(tb_c864rw, tb_c864_1, tb_c864_1n); tb_c864rw += tb_c864_1n; memcpy(tb_c864rw, tb_c864_2, tb_c864_2n); tb_c864rw += tb_c864_2n; *tb_c864rw = 0; (const char *)tb_c864r; }));
        f_emit__emit_expr(v_c, v_value);
        f_emit__put(v_c, "; tb_bset(");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 678))->q_kids, 0LL, sizeof(tb_o_Node *), 678)));
        f_emit__put(v_c, ", ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 680))->q_kids, 1LL, sizeof(tb_o_Node *), 680)));
        f_emit__put(v_c, ({ const char *tb_c865_0 = ", tb_v"; long long tb_c865_0n = sizeof(", tb_v") - 1; const char *tb_c865_1 = v_id; long long tb_c865_1n = (long long)strlen(tb_c865_1); const char *tb_c865_2 = ", "; long long tb_c865_2n = sizeof(", ") - 1; char tb_c865_3d[24]; long long tb_c865_3n; const char *tb_c865_3 = tb_digits(((tb_o_Node *)tb_nn(v_n, 681))->q_line, tb_c865_3d, &tb_c865_3n); const char *tb_c865_4 = "); }\n"; long long tb_c865_4n = sizeof("); }\n") - 1; char *tb_c865r = tb_mem(tb_c865_0n + tb_c865_1n + tb_c865_2n + tb_c865_3n + tb_c865_4n + 1), *tb_c865rw = tb_c865r; memcpy(tb_c865rw, tb_c865_0, tb_c865_0n); tb_c865rw += tb_c865_0n; memcpy(tb_c865rw, tb_c865_1, tb_c865_1n); tb_c865rw += tb_c865_1n; memcpy(tb_c865rw, tb_c865_2, tb_c865_2n); tb_c865rw += tb_c865_2n; memcpy(tb_c865rw, tb_c865_3, tb_c865_3n); tb_c865rw += tb_c865_3n; memcpy(tb_c865rw, tb_c865_4, tb_c865_4n); tb_c865rw += tb_c865_4n; *tb_c865rw = 0; (const char *)tb_c865r; }));
        return;
    }
    if ((((({ const char *tb_s866 = v_op; ((unsigned char)tb_s866[0] == 43 && (unsigned char)tb_s866[1] == 61 && tb_s866[2] == 0); }) && f_emit__is_bld(v_c, v_target)) && f_emit__is_text_plus(v_c, v_value)) && (!f_emit__bld_uses(v_value, ((tb_o_Node *)tb_nn(v_target, 683))->q_name)))) {
        f_emit__put(v_c, "} ");
        tb_o_Node * v_r = v_value;
        while (f_emit__is_text_plus(v_c, v_r)) {
            v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 688))->q_kids, 0LL, sizeof(tb_o_Node *), 688));
        }
        f_emit__emit_app(v_c, ((tb_o_Node *)tb_nn(v_target, 689))->q_name, v_r);
        {
            tb_list * tb_l867 = f_emit__app_parts(v_c, v_value);
            for (long long tb_i867 = 0; tb_i867 < tb_l867->len; tb_i867++) {
                tb_o_Node * const v_part = ((tb_o_Node * *)tb_l867->data)[tb_i867];
                f_emit__put(v_c, " ");
                f_emit__emit_app(v_c, ((tb_o_Node *)tb_nn(v_target, 692))->q_name, v_part);
            }
        }
        f_emit__put(v_c, "\n");
        return;
    }
    if ((({ const char *tb_s868 = v_op; ((unsigned char)tb_s868[0] == 43 && (unsigned char)tb_s868[1] == 61 && tb_s868[2] == 0); }) && f_emit__is_bld(v_c, v_target))) {
        f_emit__put(v_c, ({ const char *tb_c869_0 = "const char *tb_v"; long long tb_c869_0n = sizeof("const char *tb_v") - 1; const char *tb_c869_1 = v_id; long long tb_c869_1n = (long long)strlen(tb_c869_1); const char *tb_c869_2 = " = "; long long tb_c869_2n = sizeof(" = ") - 1; char *tb_c869r = tb_mem(tb_c869_0n + tb_c869_1n + tb_c869_2n + 1), *tb_c869rw = tb_c869r; memcpy(tb_c869rw, tb_c869_0, tb_c869_0n); tb_c869rw += tb_c869_0n; memcpy(tb_c869rw, tb_c869_1, tb_c869_1n); tb_c869rw += tb_c869_1n; memcpy(tb_c869rw, tb_c869_2, tb_c869_2n); tb_c869rw += tb_c869_2n; *tb_c869rw = 0; (const char *)tb_c869r; }));
        f_emit__emit_as_text(v_c, v_value);
        f_emit__put(v_c, ({ const char *tb_c870_0 = "; tb_app(&v_"; long long tb_c870_0n = sizeof("; tb_app(&v_") - 1; const char *tb_c870_1 = ((tb_o_Node *)tb_nn(v_target, 699))->q_name; long long tb_c870_1n = (long long)strlen(tb_c870_1); const char *tb_c870_2 = ", &tb_rl_"; long long tb_c870_2n = sizeof(", &tb_rl_") - 1; const char *tb_c870_3 = ((tb_o_Node *)tb_nn(v_target, 699))->q_name; long long tb_c870_3n = (long long)strlen(tb_c870_3); const char *tb_c870_4 = ", &tb_rc_"; long long tb_c870_4n = sizeof(", &tb_rc_") - 1; const char *tb_c870_5 = ((tb_o_Node *)tb_nn(v_target, 699))->q_name; long long tb_c870_5n = (long long)strlen(tb_c870_5); const char *tb_c870_6 = ", tb_v"; long long tb_c870_6n = sizeof(", tb_v") - 1; const char *tb_c870_7 = v_id; long long tb_c870_7n = (long long)strlen(tb_c870_7); const char *tb_c870_8 = "); }\n"; long long tb_c870_8n = sizeof("); }\n") - 1; char *tb_c870r = tb_mem(tb_c870_0n + tb_c870_1n + tb_c870_2n + tb_c870_3n + tb_c870_4n + tb_c870_5n + tb_c870_6n + tb_c870_7n + tb_c870_8n + 1), *tb_c870rw = tb_c870r; memcpy(tb_c870rw, tb_c870_0, tb_c870_0n); tb_c870rw += tb_c870_0n; memcpy(tb_c870rw, tb_c870_1, tb_c870_1n); tb_c870rw += tb_c870_1n; memcpy(tb_c870rw, tb_c870_2, tb_c870_2n); tb_c870rw += tb_c870_2n; memcpy(tb_c870rw, tb_c870_3, tb_c870_3n); tb_c870rw += tb_c870_3n; memcpy(tb_c870rw, tb_c870_4, tb_c870_4n); tb_c870rw += tb_c870_4n; memcpy(tb_c870rw, tb_c870_5, tb_c870_5n); tb_c870rw += tb_c870_5n; memcpy(tb_c870rw, tb_c870_6, tb_c870_6n); tb_c870rw += tb_c870_6n; memcpy(tb_c870rw, tb_c870_7, tb_c870_7n); tb_c870rw += tb_c870_7n; memcpy(tb_c870rw, tb_c870_8, tb_c870_8n); tb_c870rw += tb_c870_8n; *tb_c870rw = 0; (const char *)tb_c870r; }));
        return;
    }
    if ((({ const char *tb_s871 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 701))->q_ty); ((unsigned char)tb_s871[0] == 116 && (unsigned char)tb_s871[1] == 101 && (unsigned char)tb_s871[2] == 120 && (unsigned char)tb_s871[3] == 116 && tb_s871[4] == 0); }) && ({ const char *tb_s872 = v_op; !((unsigned char)tb_s872[0] == 61 && tb_s872[1] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c873_0 = "const char *tb_v"; long long tb_c873_0n = sizeof("const char *tb_v") - 1; const char *tb_c873_1 = v_id; long long tb_c873_1n = (long long)strlen(tb_c873_1); const char *tb_c873_2 = " = "; long long tb_c873_2n = sizeof(" = ") - 1; char *tb_c873r = tb_mem(tb_c873_0n + tb_c873_1n + tb_c873_2n + 1), *tb_c873rw = tb_c873r; memcpy(tb_c873rw, tb_c873_0, tb_c873_0n); tb_c873rw += tb_c873_0n; memcpy(tb_c873rw, tb_c873_1, tb_c873_1n); tb_c873rw += tb_c873_1n; memcpy(tb_c873rw, tb_c873_2, tb_c873_2n); tb_c873rw += tb_c873_2n; *tb_c873rw = 0; (const char *)tb_c873r; }));
        f_emit__emit_as_text(v_c, v_value);
    } else {
        if (({ const char *tb_s874 = v_op; ((unsigned char)tb_s874[0] == 61 && tb_s874[1] == 0); })) {
            f_emit__put(v_c, ({ const char *tb_c875_0 = v_t; long long tb_c875_0n = (long long)strlen(tb_c875_0); const char *tb_c875_1 = " tb_v"; long long tb_c875_1n = sizeof(" tb_v") - 1; const char *tb_c875_2 = v_id; long long tb_c875_2n = (long long)strlen(tb_c875_2); const char *tb_c875_3 = " = "; long long tb_c875_3n = sizeof(" = ") - 1; char *tb_c875r = tb_mem(tb_c875_0n + tb_c875_1n + tb_c875_2n + tb_c875_3n + 1), *tb_c875rw = tb_c875r; memcpy(tb_c875rw, tb_c875_0, tb_c875_0n); tb_c875rw += tb_c875_0n; memcpy(tb_c875rw, tb_c875_1, tb_c875_1n); tb_c875rw += tb_c875_1n; memcpy(tb_c875rw, tb_c875_2, tb_c875_2n); tb_c875rw += tb_c875_2n; memcpy(tb_c875rw, tb_c875_3, tb_c875_3n); tb_c875rw += tb_c875_3n; *tb_c875rw = 0; (const char *)tb_c875r; }));
            f_emit__emit_as(v_c, v_value, ((tb_o_Node *)tb_nn(v_n, 707))->q_ty);
        } else {
            f_emit__put(v_c, ({ const char *tb_c876_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_value, 709))->q_ty); long long tb_c876_0n = (long long)strlen(tb_c876_0); const char *tb_c876_1 = " tb_v"; long long tb_c876_1n = sizeof(" tb_v") - 1; const char *tb_c876_2 = v_id; long long tb_c876_2n = (long long)strlen(tb_c876_2); const char *tb_c876_3 = " = "; long long tb_c876_3n = sizeof(" = ") - 1; char *tb_c876r = tb_mem(tb_c876_0n + tb_c876_1n + tb_c876_2n + tb_c876_3n + 1), *tb_c876rw = tb_c876r; memcpy(tb_c876rw, tb_c876_0, tb_c876_0n); tb_c876rw += tb_c876_0n; memcpy(tb_c876rw, tb_c876_1, tb_c876_1n); tb_c876rw += tb_c876_1n; memcpy(tb_c876rw, tb_c876_2, tb_c876_2n); tb_c876rw += tb_c876_2n; memcpy(tb_c876rw, tb_c876_3, tb_c876_3n); tb_c876rw += tb_c876_3n; *tb_c876rw = 0; (const char *)tb_c876r; }));
            f_emit__emit_expr(v_c, v_value);
        }
    }
    f_emit__put(v_c, ({ const char *tb_c877_0 = "; "; long long tb_c877_0n = sizeof("; ") - 1; const char *tb_c877_1 = v_t; long long tb_c877_1n = (long long)strlen(tb_c877_1); const char *tb_c877_2 = " *tb_p"; long long tb_c877_2n = sizeof(" *tb_p") - 1; const char *tb_c877_3 = v_id; long long tb_c877_3n = (long long)strlen(tb_c877_3); const char *tb_c877_4 = " = &("; long long tb_c877_4n = sizeof(" = &(") - 1; char *tb_c877r = tb_mem(tb_c877_0n + tb_c877_1n + tb_c877_2n + tb_c877_3n + tb_c877_4n + 1), *tb_c877rw = tb_c877r; memcpy(tb_c877rw, tb_c877_0, tb_c877_0n); tb_c877rw += tb_c877_0n; memcpy(tb_c877rw, tb_c877_1, tb_c877_1n); tb_c877rw += tb_c877_1n; memcpy(tb_c877rw, tb_c877_2, tb_c877_2n); tb_c877rw += tb_c877_2n; memcpy(tb_c877rw, tb_c877_3, tb_c877_3n); tb_c877rw += tb_c877_3n; memcpy(tb_c877rw, tb_c877_4, tb_c877_4n); tb_c877rw += tb_c877_4n; *tb_c877rw = 0; (const char *)tb_c877r; }));
    f_emit__emit_target(v_c, v_target);
    f_emit__put(v_c, "); ");
    if (({ const char *tb_s878 = v_op; ((unsigned char)tb_s878[0] == 61 && tb_s878[1] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c879_0 = "*tb_p"; long long tb_c879_0n = sizeof("*tb_p") - 1; const char *tb_c879_1 = v_id; long long tb_c879_1n = (long long)strlen(tb_c879_1); const char *tb_c879_2 = " = tb_v"; long long tb_c879_2n = sizeof(" = tb_v") - 1; const char *tb_c879_3 = v_id; long long tb_c879_3n = (long long)strlen(tb_c879_3); const char *tb_c879_4 = ";"; long long tb_c879_4n = sizeof(";") - 1; char *tb_c879r = tb_mem(tb_c879_0n + tb_c879_1n + tb_c879_2n + tb_c879_3n + tb_c879_4n + 1), *tb_c879rw = tb_c879r; memcpy(tb_c879rw, tb_c879_0, tb_c879_0n); tb_c879rw += tb_c879_0n; memcpy(tb_c879rw, tb_c879_1, tb_c879_1n); tb_c879rw += tb_c879_1n; memcpy(tb_c879rw, tb_c879_2, tb_c879_2n); tb_c879rw += tb_c879_2n; memcpy(tb_c879rw, tb_c879_3, tb_c879_3n); tb_c879rw += tb_c879_3n; memcpy(tb_c879rw, tb_c879_4, tb_c879_4n); tb_c879rw += tb_c879_4n; *tb_c879rw = 0; (const char *)tb_c879r; }));
    } else if (({ const char *tb_s880 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 716))->q_ty); ((unsigned char)tb_s880[0] == 116 && (unsigned char)tb_s880[1] == 101 && (unsigned char)tb_s880[2] == 120 && (unsigned char)tb_s880[3] == 116 && tb_s880[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c881_0 = "*tb_p"; long long tb_c881_0n = sizeof("*tb_p") - 1; const char *tb_c881_1 = v_id; long long tb_c881_1n = (long long)strlen(tb_c881_1); const char *tb_c881_2 = " = tb_cat(*tb_p"; long long tb_c881_2n = sizeof(" = tb_cat(*tb_p") - 1; const char *tb_c881_3 = v_id; long long tb_c881_3n = (long long)strlen(tb_c881_3); const char *tb_c881_4 = ", tb_v"; long long tb_c881_4n = sizeof(", tb_v") - 1; const char *tb_c881_5 = v_id; long long tb_c881_5n = (long long)strlen(tb_c881_5); const char *tb_c881_6 = ");"; long long tb_c881_6n = sizeof(");") - 1; char *tb_c881r = tb_mem(tb_c881_0n + tb_c881_1n + tb_c881_2n + tb_c881_3n + tb_c881_4n + tb_c881_5n + tb_c881_6n + 1), *tb_c881rw = tb_c881r; memcpy(tb_c881rw, tb_c881_0, tb_c881_0n); tb_c881rw += tb_c881_0n; memcpy(tb_c881rw, tb_c881_1, tb_c881_1n); tb_c881rw += tb_c881_1n; memcpy(tb_c881rw, tb_c881_2, tb_c881_2n); tb_c881rw += tb_c881_2n; memcpy(tb_c881rw, tb_c881_3, tb_c881_3n); tb_c881rw += tb_c881_3n; memcpy(tb_c881rw, tb_c881_4, tb_c881_4n); tb_c881rw += tb_c881_4n; memcpy(tb_c881rw, tb_c881_5, tb_c881_5n); tb_c881rw += tb_c881_5n; memcpy(tb_c881rw, tb_c881_6, tb_c881_6n); tb_c881rw += tb_c881_6n; *tb_c881rw = 0; (const char *)tb_c881r; }));
    } else if ((({ const char *tb_s882 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 718))->q_ty); ((unsigned char)tb_s882[0] == 105 && (unsigned char)tb_s882[1] == 110 && (unsigned char)tb_s882[2] == 116 && tb_s882[3] == 0); }) && ({ const char *tb_s883 = v_op; ((unsigned char)tb_s883[0] == 47 && (unsigned char)tb_s883[1] == 61 && tb_s883[2] == 0); }))) {
        f_emit__put(v_c, ({ const char *tb_c884_0 = "*tb_p"; long long tb_c884_0n = sizeof("*tb_p") - 1; const char *tb_c884_1 = v_id; long long tb_c884_1n = (long long)strlen(tb_c884_1); const char *tb_c884_2 = " = tb_div(*tb_p"; long long tb_c884_2n = sizeof(" = tb_div(*tb_p") - 1; const char *tb_c884_3 = v_id; long long tb_c884_3n = (long long)strlen(tb_c884_3); const char *tb_c884_4 = ", tb_v"; long long tb_c884_4n = sizeof(", tb_v") - 1; const char *tb_c884_5 = v_id; long long tb_c884_5n = (long long)strlen(tb_c884_5); const char *tb_c884_6 = ", "; long long tb_c884_6n = sizeof(", ") - 1; char tb_c884_7d[24]; long long tb_c884_7n; const char *tb_c884_7 = tb_digits(((tb_o_Node *)tb_nn(v_n, 719))->q_line, tb_c884_7d, &tb_c884_7n); const char *tb_c884_8 = ");"; long long tb_c884_8n = sizeof(");") - 1; char *tb_c884r = tb_mem(tb_c884_0n + tb_c884_1n + tb_c884_2n + tb_c884_3n + tb_c884_4n + tb_c884_5n + tb_c884_6n + tb_c884_7n + tb_c884_8n + 1), *tb_c884rw = tb_c884r; memcpy(tb_c884rw, tb_c884_0, tb_c884_0n); tb_c884rw += tb_c884_0n; memcpy(tb_c884rw, tb_c884_1, tb_c884_1n); tb_c884rw += tb_c884_1n; memcpy(tb_c884rw, tb_c884_2, tb_c884_2n); tb_c884rw += tb_c884_2n; memcpy(tb_c884rw, tb_c884_3, tb_c884_3n); tb_c884rw += tb_c884_3n; memcpy(tb_c884rw, tb_c884_4, tb_c884_4n); tb_c884rw += tb_c884_4n; memcpy(tb_c884rw, tb_c884_5, tb_c884_5n); tb_c884rw += tb_c884_5n; memcpy(tb_c884rw, tb_c884_6, tb_c884_6n); tb_c884rw += tb_c884_6n; memcpy(tb_c884rw, tb_c884_7, tb_c884_7n); tb_c884rw += tb_c884_7n; memcpy(tb_c884rw, tb_c884_8, tb_c884_8n); tb_c884rw += tb_c884_8n; *tb_c884rw = 0; (const char *)tb_c884r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c885_0 = "*tb_p"; long long tb_c885_0n = sizeof("*tb_p") - 1; const char *tb_c885_1 = v_id; long long tb_c885_1n = (long long)strlen(tb_c885_1); const char *tb_c885_2 = " "; long long tb_c885_2n = sizeof(" ") - 1; const char *tb_c885_3 = v_op; long long tb_c885_3n = (long long)strlen(tb_c885_3); const char *tb_c885_4 = " tb_v"; long long tb_c885_4n = sizeof(" tb_v") - 1; const char *tb_c885_5 = v_id; long long tb_c885_5n = (long long)strlen(tb_c885_5); const char *tb_c885_6 = ";"; long long tb_c885_6n = sizeof(";") - 1; char *tb_c885r = tb_mem(tb_c885_0n + tb_c885_1n + tb_c885_2n + tb_c885_3n + tb_c885_4n + tb_c885_5n + tb_c885_6n + 1), *tb_c885rw = tb_c885r; memcpy(tb_c885rw, tb_c885_0, tb_c885_0n); tb_c885rw += tb_c885_0n; memcpy(tb_c885rw, tb_c885_1, tb_c885_1n); tb_c885rw += tb_c885_1n; memcpy(tb_c885rw, tb_c885_2, tb_c885_2n); tb_c885rw += tb_c885_2n; memcpy(tb_c885rw, tb_c885_3, tb_c885_3n); tb_c885rw += tb_c885_3n; memcpy(tb_c885rw, tb_c885_4, tb_c885_4n); tb_c885rw += tb_c885_4n; memcpy(tb_c885rw, tb_c885_5, tb_c885_5n); tb_c885rw += tb_c885_5n; memcpy(tb_c885rw, tb_c885_6, tb_c885_6n); tb_c885rw += tb_c885_6n; *tb_c885rw = 0; (const char *)tb_c885r; }));
    }
    f_emit__put(v_c, " }\n");
}

static void f_emit__emit_multi(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    long long v_names = ((((tb_o_Node *)tb_nn(v_n, 725))->q_kids)->len - 1LL);
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 726))->q_kids, v_names, sizeof(tb_o_Node *), 726));
    f_emit__emit_pre(v_c, v_call, v_depth);
    { long long tb_v886 = 1LL; long long *tb_p886 = &(((tb_o_Comp *)tb_nn(v_c, 728))->q_label); *tb_p886 += tb_v886; }
    const char * v_id = ({ const char *tb_c887_0 = ""; long long tb_c887_0n = sizeof("") - 1; char tb_c887_1d[24]; long long tb_c887_1n; const char *tb_c887_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 729))->q_label, tb_c887_1d, &tb_c887_1n); char *tb_c887r = tb_mem(tb_c887_0n + tb_c887_1n + 1), *tb_c887rw = tb_c887r; memcpy(tb_c887rw, tb_c887_0, tb_c887_0n); tb_c887rw += tb_c887_0n; memcpy(tb_c887rw, tb_c887_1, tb_c887_1n); tb_c887rw += tb_c887_1n; *tb_c887rw = 0; (const char *)tb_c887r; });
    {
        long long tb_end888 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end888; v_i++) {
            if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 731))->q_kids, v_i, sizeof(tb_o_Node *), 731)), 731))->q_creates) {
                f_emit__indent(v_c, v_depth);
                f_emit__put(v_c, ({ const char *tb_c889_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 733))->q_kids, v_i, sizeof(tb_o_Node *), 733)), 733))->q_ty); long long tb_c889_0n = (long long)strlen(tb_c889_0); const char *tb_c889_1 = " v_"; long long tb_c889_1n = sizeof(" v_") - 1; const char *tb_c889_2 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 733))->q_kids, v_i, sizeof(tb_o_Node *), 733)), 733))->q_name; long long tb_c889_2n = (long long)strlen(tb_c889_2); const char *tb_c889_3 = ";\n"; long long tb_c889_3n = sizeof(";\n") - 1; char *tb_c889r = tb_mem(tb_c889_0n + tb_c889_1n + tb_c889_2n + tb_c889_3n + 1), *tb_c889rw = tb_c889r; memcpy(tb_c889rw, tb_c889_0, tb_c889_0n); tb_c889rw += tb_c889_0n; memcpy(tb_c889rw, tb_c889_1, tb_c889_1n); tb_c889rw += tb_c889_1n; memcpy(tb_c889rw, tb_c889_2, tb_c889_2n); tb_c889rw += tb_c889_2n; memcpy(tb_c889rw, tb_c889_3, tb_c889_3n); tb_c889rw += tb_c889_3n; *tb_c889rw = 0; (const char *)tb_c889r; }));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c890_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_call, 737))->q_ty); long long tb_c890_0n = (long long)strlen(tb_c890_0); const char *tb_c890_1 = " tb_m"; long long tb_c890_1n = sizeof(" tb_m") - 1; const char *tb_c890_2 = v_id; long long tb_c890_2n = (long long)strlen(tb_c890_2); const char *tb_c890_3 = " = "; long long tb_c890_3n = sizeof(" = ") - 1; char *tb_c890r = tb_mem(tb_c890_0n + tb_c890_1n + tb_c890_2n + tb_c890_3n + 1), *tb_c890rw = tb_c890r; memcpy(tb_c890rw, tb_c890_0, tb_c890_0n); tb_c890rw += tb_c890_0n; memcpy(tb_c890rw, tb_c890_1, tb_c890_1n); tb_c890rw += tb_c890_1n; memcpy(tb_c890rw, tb_c890_2, tb_c890_2n); tb_c890rw += tb_c890_2n; memcpy(tb_c890rw, tb_c890_3, tb_c890_3n); tb_c890rw += tb_c890_3n; *tb_c890rw = 0; (const char *)tb_c890r; }));
    f_emit__emit_expr(v_c, v_call);
    f_emit__put(v_c, ";\n");
    {
        long long tb_end891 = (v_names - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end891; v_i++) {
            f_emit__indent(v_c, (v_depth + 1LL));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 742))->q_kids, v_i, sizeof(tb_o_Node *), 742)));
            f_emit__put(v_c, ({ const char *tb_c892_0 = " = tb_m"; long long tb_c892_0n = sizeof(" = tb_m") - 1; const char *tb_c892_1 = v_id; long long tb_c892_1n = (long long)strlen(tb_c892_1); const char *tb_c892_2 = ".o"; long long tb_c892_2n = sizeof(".o") - 1; char tb_c892_3d[24]; long long tb_c892_3n; const char *tb_c892_3 = tb_digits(v_i, tb_c892_3d, &tb_c892_3n); const char *tb_c892_4 = ";\n"; long long tb_c892_4n = sizeof(";\n") - 1; char *tb_c892r = tb_mem(tb_c892_0n + tb_c892_1n + tb_c892_2n + tb_c892_3n + tb_c892_4n + 1), *tb_c892rw = tb_c892r; memcpy(tb_c892rw, tb_c892_0, tb_c892_0n); tb_c892rw += tb_c892_0n; memcpy(tb_c892rw, tb_c892_1, tb_c892_1n); tb_c892rw += tb_c892_1n; memcpy(tb_c892rw, tb_c892_2, tb_c892_2n); tb_c892rw += tb_c892_2n; memcpy(tb_c892rw, tb_c892_3, tb_c892_3n); tb_c892rw += tb_c892_3n; memcpy(tb_c892rw, tb_c892_4, tb_c892_4n); tb_c892rw += tb_c892_4n; *tb_c892rw = 0; (const char *)tb_c892r; }));
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
        { long long tb_v893 = 2LL; long long *tb_p893 = &(v_i); *tb_p893 += tb_v893; }
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
    { long long tb_v894 = 1LL; long long *tb_p894 = &(((tb_o_Comp *)tb_nn(v_c, 771))->q_label); *tb_p894 += tb_v894; }
    const char * v_id = ({ const char *tb_c895_0 = ""; long long tb_c895_0n = sizeof("") - 1; char tb_c895_1d[24]; long long tb_c895_1n; const char *tb_c895_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 772))->q_label, tb_c895_1d, &tb_c895_1n); char *tb_c895r = tb_mem(tb_c895_0n + tb_c895_1n + 1), *tb_c895rw = tb_c895r; memcpy(tb_c895rw, tb_c895_0, tb_c895_0n); tb_c895rw += tb_c895_0n; memcpy(tb_c895rw, tb_c895_1, tb_c895_1n); tb_c895rw += tb_c895_1n; *tb_c895rw = 0; (const char *)tb_c895r; });
    _Bool v_is_each = ({ const char *tb_s896 = ((tb_o_Node *)tb_nn(v_n, 773))->q_kind; ((unsigned char)tb_s896[0] == 101 && (unsigned char)tb_s896[1] == 97 && (unsigned char)tb_s896[2] == 99 && (unsigned char)tb_s896[3] == 104 && tb_s896[4] == 0); });
    const char * v_elem = "";
    if (v_is_each) {
        v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 776))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 776))->q_kids, 0LL, sizeof(tb_o_Node *), 776)), 776))->q_ty, sizeof(tb_o_Type *), 776)), 776))->q_elem);
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 778))->q_out;
    tb_list * v_w = tb_list_new(sizeof(const char *));
    { tb_list * tb_v897 = v_w; tb_list * *tb_p897 = &(((tb_o_Comp *)tb_nn(v_c, 780))->q_out); *tb_p897 = tb_v897; }
    const char * v_list_type = "tb_list *";
    if (v_is_each) {
        v_list_type = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 783))->q_kids, 0LL, sizeof(tb_o_Node *), 783)), 783))->q_ty);
    }
    f_emit__put(v_c, ({ const char *tb_c898_0 = "\ntypedef struct {\n    long long tb_lo, tb_total, tb_step, tb_next;\n    "; long long tb_c898_0n = sizeof("\ntypedef struct {\n    long long tb_lo, tb_total, tb_step, tb_next;\n    ") - 1; const char *tb_c898_1 = v_list_type; long long tb_c898_1n = (long long)strlen(tb_c898_1); const char *tb_c898_2 = " tb_l;\n"; long long tb_c898_2n = sizeof(" tb_l;\n") - 1; char *tb_c898r = tb_mem(tb_c898_0n + tb_c898_1n + tb_c898_2n + 1), *tb_c898rw = tb_c898r; memcpy(tb_c898rw, tb_c898_0, tb_c898_0n); tb_c898rw += tb_c898_0n; memcpy(tb_c898rw, tb_c898_1, tb_c898_1n); tb_c898rw += tb_c898_1n; memcpy(tb_c898rw, tb_c898_2, tb_c898_2n); tb_c898rw += tb_c898_2n; *tb_c898rw = 0; (const char *)tb_c898r; }));
    {
        tb_list * tb_l899 = ((tb_o_Node *)tb_nn(v_n, 785))->q_caps;
        for (long long tb_i899 = 0; tb_i899 < tb_l899->len; tb_i899++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l899->data)[tb_i899];
            if ((((tb_o_Var *)tb_nn(v_v, 786))->q_is_shared && ({ const char *tb_s900 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 786))->q_type); ((unsigned char)tb_s900[0] == 116 && (unsigned char)tb_s900[1] == 101 && (unsigned char)tb_s900[2] == 120 && (unsigned char)tb_s900[3] == 116 && tb_s900[4] == 0); }))) {
                f_emit__put(v_c, ({ const char *tb_c901_0 = "    const char *m_"; long long tb_c901_0n = sizeof("    const char *m_") - 1; const char *tb_c901_1 = ((tb_o_Var *)tb_nn(v_v, 788))->q_name; long long tb_c901_1n = (long long)strlen(tb_c901_1); const char *tb_c901_2 = "; long long m_rl_"; long long tb_c901_2n = sizeof("; long long m_rl_") - 1; const char *tb_c901_3 = ((tb_o_Var *)tb_nn(v_v, 788))->q_name; long long tb_c901_3n = (long long)strlen(tb_c901_3); const char *tb_c901_4 = ", m_rc_"; long long tb_c901_4n = sizeof(", m_rc_") - 1; const char *tb_c901_5 = ((tb_o_Var *)tb_nn(v_v, 788))->q_name; long long tb_c901_5n = (long long)strlen(tb_c901_5); const char *tb_c901_6 = ";\n"; long long tb_c901_6n = sizeof(";\n") - 1; char *tb_c901r = tb_mem(tb_c901_0n + tb_c901_1n + tb_c901_2n + tb_c901_3n + tb_c901_4n + tb_c901_5n + tb_c901_6n + 1), *tb_c901rw = tb_c901r; memcpy(tb_c901rw, tb_c901_0, tb_c901_0n); tb_c901rw += tb_c901_0n; memcpy(tb_c901rw, tb_c901_1, tb_c901_1n); tb_c901rw += tb_c901_1n; memcpy(tb_c901rw, tb_c901_2, tb_c901_2n); tb_c901rw += tb_c901_2n; memcpy(tb_c901rw, tb_c901_3, tb_c901_3n); tb_c901rw += tb_c901_3n; memcpy(tb_c901rw, tb_c901_4, tb_c901_4n); tb_c901rw += tb_c901_4n; memcpy(tb_c901rw, tb_c901_5, tb_c901_5n); tb_c901rw += tb_c901_5n; memcpy(tb_c901rw, tb_c901_6, tb_c901_6n); tb_c901rw += tb_c901_6n; *tb_c901rw = 0; (const char *)tb_c901r; }));
            } else if (((tb_o_Var *)tb_nn(v_v, 789))->q_is_shared) {
                f_emit__put(v_c, ({ const char *tb_c902_0 = "    "; long long tb_c902_0n = sizeof("    ") - 1; const char *tb_c902_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 790))->q_type); long long tb_c902_1n = (long long)strlen(tb_c902_1); const char *tb_c902_2 = " *p_"; long long tb_c902_2n = sizeof(" *p_") - 1; const char *tb_c902_3 = ((tb_o_Var *)tb_nn(v_v, 790))->q_name; long long tb_c902_3n = (long long)strlen(tb_c902_3); const char *tb_c902_4 = ";\n"; long long tb_c902_4n = sizeof(";\n") - 1; char *tb_c902r = tb_mem(tb_c902_0n + tb_c902_1n + tb_c902_2n + tb_c902_3n + tb_c902_4n + 1), *tb_c902rw = tb_c902r; memcpy(tb_c902rw, tb_c902_0, tb_c902_0n); tb_c902rw += tb_c902_0n; memcpy(tb_c902rw, tb_c902_1, tb_c902_1n); tb_c902rw += tb_c902_1n; memcpy(tb_c902rw, tb_c902_2, tb_c902_2n); tb_c902rw += tb_c902_2n; memcpy(tb_c902rw, tb_c902_3, tb_c902_3n); tb_c902rw += tb_c902_3n; memcpy(tb_c902rw, tb_c902_4, tb_c902_4n); tb_c902rw += tb_c902_4n; *tb_c902rw = 0; (const char *)tb_c902r; }));
            } else if (((tb_o_Var *)tb_nn(v_v, 791))->q_is_link) {
                f_emit__put(v_c, ({ const char *tb_c903_0 = "    "; long long tb_c903_0n = sizeof("    ") - 1; const char *tb_c903_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 792))->q_type); long long tb_c903_1n = (long long)strlen(tb_c903_1); const char *tb_c903_2 = " *v_"; long long tb_c903_2n = sizeof(" *v_") - 1; const char *tb_c903_3 = ((tb_o_Var *)tb_nn(v_v, 792))->q_name; long long tb_c903_3n = (long long)strlen(tb_c903_3); const char *tb_c903_4 = ";\n"; long long tb_c903_4n = sizeof(";\n") - 1; char *tb_c903r = tb_mem(tb_c903_0n + tb_c903_1n + tb_c903_2n + tb_c903_3n + tb_c903_4n + 1), *tb_c903rw = tb_c903r; memcpy(tb_c903rw, tb_c903_0, tb_c903_0n); tb_c903rw += tb_c903_0n; memcpy(tb_c903rw, tb_c903_1, tb_c903_1n); tb_c903rw += tb_c903_1n; memcpy(tb_c903rw, tb_c903_2, tb_c903_2n); tb_c903rw += tb_c903_2n; memcpy(tb_c903rw, tb_c903_3, tb_c903_3n); tb_c903rw += tb_c903_3n; memcpy(tb_c903rw, tb_c903_4, tb_c903_4n); tb_c903rw += tb_c903_4n; *tb_c903rw = 0; (const char *)tb_c903r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c904_0 = "    "; long long tb_c904_0n = sizeof("    ") - 1; const char *tb_c904_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 794))->q_type); long long tb_c904_1n = (long long)strlen(tb_c904_1); const char *tb_c904_2 = " v_"; long long tb_c904_2n = sizeof(" v_") - 1; const char *tb_c904_3 = ((tb_o_Var *)tb_nn(v_v, 794))->q_name; long long tb_c904_3n = (long long)strlen(tb_c904_3); const char *tb_c904_4 = ";\n"; long long tb_c904_4n = sizeof(";\n") - 1; char *tb_c904r = tb_mem(tb_c904_0n + tb_c904_1n + tb_c904_2n + tb_c904_3n + tb_c904_4n + 1), *tb_c904rw = tb_c904r; memcpy(tb_c904rw, tb_c904_0, tb_c904_0n); tb_c904rw += tb_c904_0n; memcpy(tb_c904rw, tb_c904_1, tb_c904_1n); tb_c904rw += tb_c904_1n; memcpy(tb_c904rw, tb_c904_2, tb_c904_2n); tb_c904rw += tb_c904_2n; memcpy(tb_c904rw, tb_c904_3, tb_c904_3n); tb_c904rw += tb_c904_3n; memcpy(tb_c904rw, tb_c904_4, tb_c904_4n); tb_c904rw += tb_c904_4n; *tb_c904rw = 0; (const char *)tb_c904r; }));
            }
        }
    }
    f_emit__put(v_c, ({ const char *tb_c905_0 = "} tb_job"; long long tb_c905_0n = sizeof("} tb_job") - 1; const char *tb_c905_1 = v_id; long long tb_c905_1n = (long long)strlen(tb_c905_1); const char *tb_c905_2 = ";\n"; long long tb_c905_2n = sizeof(";\n") - 1; char *tb_c905r = tb_mem(tb_c905_0n + tb_c905_1n + tb_c905_2n + 1), *tb_c905rw = tb_c905r; memcpy(tb_c905rw, tb_c905_0, tb_c905_0n); tb_c905rw += tb_c905_0n; memcpy(tb_c905rw, tb_c905_1, tb_c905_1n); tb_c905rw += tb_c905_1n; memcpy(tb_c905rw, tb_c905_2, tb_c905_2n); tb_c905rw += tb_c905_2n; *tb_c905rw = 0; (const char *)tb_c905r; }));
    f_emit__put(v_c, ({ const char *tb_c906_0 = "static void *tb_work"; long long tb_c906_0n = sizeof("static void *tb_work") - 1; const char *tb_c906_1 = v_id; long long tb_c906_1n = (long long)strlen(tb_c906_1); const char *tb_c906_2 = "(void *tb_arg) {\n"; long long tb_c906_2n = sizeof("(void *tb_arg) {\n") - 1; char *tb_c906r = tb_mem(tb_c906_0n + tb_c906_1n + tb_c906_2n + 1), *tb_c906rw = tb_c906r; memcpy(tb_c906rw, tb_c906_0, tb_c906_0n); tb_c906rw += tb_c906_0n; memcpy(tb_c906rw, tb_c906_1, tb_c906_1n); tb_c906rw += tb_c906_1n; memcpy(tb_c906rw, tb_c906_2, tb_c906_2n); tb_c906rw += tb_c906_2n; *tb_c906rw = 0; (const char *)tb_c906r; }));
    f_emit__put(v_c, ({ const char *tb_c907_0 = "    tb_job"; long long tb_c907_0n = sizeof("    tb_job") - 1; const char *tb_c907_1 = v_id; long long tb_c907_1n = (long long)strlen(tb_c907_1); const char *tb_c907_2 = " *tb_j = tb_arg;\n"; long long tb_c907_2n = sizeof(" *tb_j = tb_arg;\n") - 1; char *tb_c907r = tb_mem(tb_c907_0n + tb_c907_1n + tb_c907_2n + 1), *tb_c907rw = tb_c907r; memcpy(tb_c907rw, tb_c907_0, tb_c907_0n); tb_c907rw += tb_c907_0n; memcpy(tb_c907rw, tb_c907_1, tb_c907_1n); tb_c907rw += tb_c907_1n; memcpy(tb_c907rw, tb_c907_2, tb_c907_2n); tb_c907rw += tb_c907_2n; *tb_c907rw = 0; (const char *)tb_c907r; }));
    _Bool v_has_shared = 0;
    tb_list * v_saved_builders = ((tb_o_Comp *)tb_nn(v_c, 800))->q_builders;
    tb_list * v_par_builders = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l908 = ((tb_o_Node *)tb_nn(v_n, 802))->q_caps;
        for (long long tb_i908 = 0; tb_i908 < tb_l908->len; tb_i908++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l908->data)[tb_i908];
            if (((tb_o_Var *)tb_nn(v_v, 803))->q_is_shared) {
                v_has_shared = 1;
                f_emit__put(v_c, ({ const char *tb_c909_0 = "    "; long long tb_c909_0n = sizeof("    ") - 1; const char *tb_c909_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 805))->q_type); long long tb_c909_1n = (long long)strlen(tb_c909_1); const char *tb_c909_2 = " v_"; long long tb_c909_2n = sizeof(" v_") - 1; const char *tb_c909_3 = ((tb_o_Var *)tb_nn(v_v, 805))->q_name; long long tb_c909_3n = (long long)strlen(tb_c909_3); const char *tb_c909_4 = " = "; long long tb_c909_4n = sizeof(" = ") - 1; char *tb_c909r = tb_mem(tb_c909_0n + tb_c909_1n + tb_c909_2n + tb_c909_3n + tb_c909_4n + 1), *tb_c909rw = tb_c909r; memcpy(tb_c909rw, tb_c909_0, tb_c909_0n); tb_c909rw += tb_c909_0n; memcpy(tb_c909rw, tb_c909_1, tb_c909_1n); tb_c909rw += tb_c909_1n; memcpy(tb_c909rw, tb_c909_2, tb_c909_2n); tb_c909rw += tb_c909_2n; memcpy(tb_c909rw, tb_c909_3, tb_c909_3n); tb_c909rw += tb_c909_3n; memcpy(tb_c909rw, tb_c909_4, tb_c909_4n); tb_c909rw += tb_c909_4n; *tb_c909rw = 0; (const char *)tb_c909r; }));
                f_emit__emit_default(v_c, ((tb_o_Var *)tb_nn(v_v, 806))->q_type);
                f_emit__put(v_c, ";\n");
                if (({ const char *tb_s910 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 808))->q_type); ((unsigned char)tb_s910[0] == 116 && (unsigned char)tb_s910[1] == 101 && (unsigned char)tb_s910[2] == 120 && (unsigned char)tb_s910[3] == 116 && tb_s910[4] == 0); })) {
                    { const char * tb_v911 = ((tb_o_Var *)tb_nn(v_v, 809))->q_name; *(const char * *)tb_grow(v_par_builders) = tb_v911; }
                    f_emit__put(v_c, ({ const char *tb_c912_0 = "    long long tb_rl_"; long long tb_c912_0n = sizeof("    long long tb_rl_") - 1; const char *tb_c912_1 = ((tb_o_Var *)tb_nn(v_v, 810))->q_name; long long tb_c912_1n = (long long)strlen(tb_c912_1); const char *tb_c912_2 = " = -1, tb_rc_"; long long tb_c912_2n = sizeof(" = -1, tb_rc_") - 1; const char *tb_c912_3 = ((tb_o_Var *)tb_nn(v_v, 810))->q_name; long long tb_c912_3n = (long long)strlen(tb_c912_3); const char *tb_c912_4 = " = 0;\n"; long long tb_c912_4n = sizeof(" = 0;\n") - 1; char *tb_c912r = tb_mem(tb_c912_0n + tb_c912_1n + tb_c912_2n + tb_c912_3n + tb_c912_4n + 1), *tb_c912rw = tb_c912r; memcpy(tb_c912rw, tb_c912_0, tb_c912_0n); tb_c912rw += tb_c912_0n; memcpy(tb_c912rw, tb_c912_1, tb_c912_1n); tb_c912rw += tb_c912_1n; memcpy(tb_c912rw, tb_c912_2, tb_c912_2n); tb_c912rw += tb_c912_2n; memcpy(tb_c912rw, tb_c912_3, tb_c912_3n); tb_c912rw += tb_c912_3n; memcpy(tb_c912rw, tb_c912_4, tb_c912_4n); tb_c912rw += tb_c912_4n; *tb_c912rw = 0; (const char *)tb_c912r; }));
                }
            } else if (((tb_o_Var *)tb_nn(v_v, 811))->q_is_link) {
                f_emit__put(v_c, ({ const char *tb_c913_0 = "    "; long long tb_c913_0n = sizeof("    ") - 1; const char *tb_c913_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 812))->q_type); long long tb_c913_1n = (long long)strlen(tb_c913_1); const char *tb_c913_2 = " *v_"; long long tb_c913_2n = sizeof(" *v_") - 1; const char *tb_c913_3 = ((tb_o_Var *)tb_nn(v_v, 812))->q_name; long long tb_c913_3n = (long long)strlen(tb_c913_3); const char *tb_c913_4 = " = tb_j->v_"; long long tb_c913_4n = sizeof(" = tb_j->v_") - 1; const char *tb_c913_5 = ((tb_o_Var *)tb_nn(v_v, 812))->q_name; long long tb_c913_5n = (long long)strlen(tb_c913_5); const char *tb_c913_6 = ";\n"; long long tb_c913_6n = sizeof(";\n") - 1; char *tb_c913r = tb_mem(tb_c913_0n + tb_c913_1n + tb_c913_2n + tb_c913_3n + tb_c913_4n + tb_c913_5n + tb_c913_6n + 1), *tb_c913rw = tb_c913r; memcpy(tb_c913rw, tb_c913_0, tb_c913_0n); tb_c913rw += tb_c913_0n; memcpy(tb_c913rw, tb_c913_1, tb_c913_1n); tb_c913rw += tb_c913_1n; memcpy(tb_c913rw, tb_c913_2, tb_c913_2n); tb_c913rw += tb_c913_2n; memcpy(tb_c913rw, tb_c913_3, tb_c913_3n); tb_c913rw += tb_c913_3n; memcpy(tb_c913rw, tb_c913_4, tb_c913_4n); tb_c913rw += tb_c913_4n; memcpy(tb_c913rw, tb_c913_5, tb_c913_5n); tb_c913rw += tb_c913_5n; memcpy(tb_c913rw, tb_c913_6, tb_c913_6n); tb_c913rw += tb_c913_6n; *tb_c913rw = 0; (const char *)tb_c913r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c914_0 = "    "; long long tb_c914_0n = sizeof("    ") - 1; const char *tb_c914_1 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 814))->q_type); long long tb_c914_1n = (long long)strlen(tb_c914_1); const char *tb_c914_2 = " v_"; long long tb_c914_2n = sizeof(" v_") - 1; const char *tb_c914_3 = ((tb_o_Var *)tb_nn(v_v, 814))->q_name; long long tb_c914_3n = (long long)strlen(tb_c914_3); const char *tb_c914_4 = " = tb_j->v_"; long long tb_c914_4n = sizeof(" = tb_j->v_") - 1; const char *tb_c914_5 = ((tb_o_Var *)tb_nn(v_v, 814))->q_name; long long tb_c914_5n = (long long)strlen(tb_c914_5); const char *tb_c914_6 = ";\n"; long long tb_c914_6n = sizeof(";\n") - 1; char *tb_c914r = tb_mem(tb_c914_0n + tb_c914_1n + tb_c914_2n + tb_c914_3n + tb_c914_4n + tb_c914_5n + tb_c914_6n + 1), *tb_c914rw = tb_c914r; memcpy(tb_c914rw, tb_c914_0, tb_c914_0n); tb_c914rw += tb_c914_0n; memcpy(tb_c914rw, tb_c914_1, tb_c914_1n); tb_c914rw += tb_c914_1n; memcpy(tb_c914rw, tb_c914_2, tb_c914_2n); tb_c914rw += tb_c914_2n; memcpy(tb_c914rw, tb_c914_3, tb_c914_3n); tb_c914rw += tb_c914_3n; memcpy(tb_c914rw, tb_c914_4, tb_c914_4n); tb_c914rw += tb_c914_4n; memcpy(tb_c914rw, tb_c914_5, tb_c914_5n); tb_c914rw += tb_c914_5n; memcpy(tb_c914rw, tb_c914_6, tb_c914_6n); tb_c914rw += tb_c914_6n; *tb_c914rw = 0; (const char *)tb_c914r; }));
            }
        }
    }
    { tb_list * tb_v915 = v_par_builders; tb_list * *tb_p915 = &(((tb_o_Comp *)tb_nn(v_c, 815))->q_builders); *tb_p915 = tb_v915; }
    f_emit__put(v_c, "    for (;;) {\n");
    f_emit__put(v_c, "        long long tb_s = __atomic_fetch_add(&tb_j->tb_next, tb_j->tb_step, __ATOMIC_RELAXED);\n");
    f_emit__put(v_c, "        if (tb_s >= tb_j->tb_total) break;\n");
    f_emit__put(v_c, "        long long tb_e = tb_s + tb_j->tb_step;\n");
    f_emit__put(v_c, "        if (tb_e > tb_j->tb_total) tb_e = tb_j->tb_total;\n");
    f_emit__put(v_c, "        for (long long tb_i = tb_s; tb_i < tb_e; tb_i++) {\n");
    if ((v_is_each && f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 822))->q_kids, 0LL, sizeof(tb_o_Node *), 822)), 822))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c916_0 = "            "; long long tb_c916_0n = sizeof("            ") - 1; const char *tb_c916_1 = v_elem; long long tb_c916_1n = (long long)strlen(tb_c916_1); const char *tb_c916_2 = " const v_"; long long tb_c916_2n = sizeof(" const v_") - 1; const char *tb_c916_3 = ((tb_o_Node *)tb_nn(v_n, 823))->q_name; long long tb_c916_3n = (long long)strlen(tb_c916_3); const char *tb_c916_4 = " = {tb_j->tb_l, tb_i};\n"; long long tb_c916_4n = sizeof(" = {tb_j->tb_l, tb_i};\n") - 1; char *tb_c916r = tb_mem(tb_c916_0n + tb_c916_1n + tb_c916_2n + tb_c916_3n + tb_c916_4n + 1), *tb_c916rw = tb_c916r; memcpy(tb_c916rw, tb_c916_0, tb_c916_0n); tb_c916rw += tb_c916_0n; memcpy(tb_c916rw, tb_c916_1, tb_c916_1n); tb_c916rw += tb_c916_1n; memcpy(tb_c916rw, tb_c916_2, tb_c916_2n); tb_c916rw += tb_c916_2n; memcpy(tb_c916rw, tb_c916_3, tb_c916_3n); tb_c916rw += tb_c916_3n; memcpy(tb_c916rw, tb_c916_4, tb_c916_4n); tb_c916rw += tb_c916_4n; *tb_c916rw = 0; (const char *)tb_c916r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 824))->q_kids, 1LL, sizeof(tb_o_Node *), 824)), 3LL);
    } else if ((v_is_each && f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 825))->q_kids, 0LL, sizeof(tb_o_Node *), 825)), 825))->q_ty))) {
        f_emit__put(v_c, ({ const char *tb_c917_0 = "            _Bool const v_"; long long tb_c917_0n = sizeof("            _Bool const v_") - 1; const char *tb_c917_1 = ((tb_o_Node *)tb_nn(v_n, 826))->q_name; long long tb_c917_1n = (long long)strlen(tb_c917_1); const char *tb_c917_2 = " = TB_BIT(tb_j->tb_l, tb_i);\n"; long long tb_c917_2n = sizeof(" = TB_BIT(tb_j->tb_l, tb_i);\n") - 1; char *tb_c917r = tb_mem(tb_c917_0n + tb_c917_1n + tb_c917_2n + 1), *tb_c917rw = tb_c917r; memcpy(tb_c917rw, tb_c917_0, tb_c917_0n); tb_c917rw += tb_c917_0n; memcpy(tb_c917rw, tb_c917_1, tb_c917_1n); tb_c917rw += tb_c917_1n; memcpy(tb_c917rw, tb_c917_2, tb_c917_2n); tb_c917rw += tb_c917_2n; *tb_c917rw = 0; (const char *)tb_c917r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 827))->q_kids, 1LL, sizeof(tb_o_Node *), 827)), 3LL);
    } else if (v_is_each) {
        f_emit__put(v_c, ({ const char *tb_c918_0 = "            "; long long tb_c918_0n = sizeof("            ") - 1; const char *tb_c918_1 = v_elem; long long tb_c918_1n = (long long)strlen(tb_c918_1); const char *tb_c918_2 = " const v_"; long long tb_c918_2n = sizeof(" const v_") - 1; const char *tb_c918_3 = ((tb_o_Node *)tb_nn(v_n, 829))->q_name; long long tb_c918_3n = (long long)strlen(tb_c918_3); const char *tb_c918_4 = " = (("; long long tb_c918_4n = sizeof(" = ((") - 1; const char *tb_c918_5 = v_elem; long long tb_c918_5n = (long long)strlen(tb_c918_5); const char *tb_c918_6 = " *)tb_j->tb_l->data)[tb_i];\n"; long long tb_c918_6n = sizeof(" *)tb_j->tb_l->data)[tb_i];\n") - 1; char *tb_c918r = tb_mem(tb_c918_0n + tb_c918_1n + tb_c918_2n + tb_c918_3n + tb_c918_4n + tb_c918_5n + tb_c918_6n + 1), *tb_c918rw = tb_c918r; memcpy(tb_c918rw, tb_c918_0, tb_c918_0n); tb_c918rw += tb_c918_0n; memcpy(tb_c918rw, tb_c918_1, tb_c918_1n); tb_c918rw += tb_c918_1n; memcpy(tb_c918rw, tb_c918_2, tb_c918_2n); tb_c918rw += tb_c918_2n; memcpy(tb_c918rw, tb_c918_3, tb_c918_3n); tb_c918rw += tb_c918_3n; memcpy(tb_c918rw, tb_c918_4, tb_c918_4n); tb_c918rw += tb_c918_4n; memcpy(tb_c918rw, tb_c918_5, tb_c918_5n); tb_c918rw += tb_c918_5n; memcpy(tb_c918rw, tb_c918_6, tb_c918_6n); tb_c918rw += tb_c918_6n; *tb_c918rw = 0; (const char *)tb_c918r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 830))->q_kids, 1LL, sizeof(tb_o_Node *), 830)), 3LL);
    } else {
        f_emit__put(v_c, ({ const char *tb_c919_0 = "            long long const v_"; long long tb_c919_0n = sizeof("            long long const v_") - 1; const char *tb_c919_1 = ((tb_o_Node *)tb_nn(v_n, 832))->q_name; long long tb_c919_1n = (long long)strlen(tb_c919_1); const char *tb_c919_2 = " = tb_j->tb_lo + tb_i;\n"; long long tb_c919_2n = sizeof(" = tb_j->tb_lo + tb_i;\n") - 1; char *tb_c919r = tb_mem(tb_c919_0n + tb_c919_1n + tb_c919_2n + 1), *tb_c919rw = tb_c919r; memcpy(tb_c919rw, tb_c919_0, tb_c919_0n); tb_c919rw += tb_c919_0n; memcpy(tb_c919rw, tb_c919_1, tb_c919_1n); tb_c919rw += tb_c919_1n; memcpy(tb_c919rw, tb_c919_2, tb_c919_2n); tb_c919rw += tb_c919_2n; *tb_c919rw = 0; (const char *)tb_c919r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 833))->q_kids, 2LL, sizeof(tb_o_Node *), 833)), 3LL);
    }
    f_emit__put(v_c, "        }\n    }\n");
    { tb_list * tb_v920 = v_saved_builders; tb_list * *tb_p920 = &(((tb_o_Comp *)tb_nn(v_c, 835))->q_builders); *tb_p920 = tb_v920; }
    if (v_has_shared) {
        f_emit__put(v_c, "    pthread_mutex_lock(&tb_par_lock);\n");
        {
            tb_list * tb_l921 = ((tb_o_Node *)tb_nn(v_n, 839))->q_caps;
            for (long long tb_i921 = 0; tb_i921 < tb_l921->len; tb_i921++) {
                tb_o_Var * const v_v = ((tb_o_Var * *)tb_l921->data)[tb_i921];
                if ((((tb_o_Var *)tb_nn(v_v, 840))->q_is_shared && ({ const char *tb_s922 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 840))->q_type); ((unsigned char)tb_s922[0] == 116 && (unsigned char)tb_s922[1] == 101 && (unsigned char)tb_s922[2] == 120 && (unsigned char)tb_s922[3] == 116 && tb_s922[4] == 0); }))) {
                    f_emit__put(v_c, ({ const char *tb_c923_0 = "    tb_app_take(&tb_j->m_"; long long tb_c923_0n = sizeof("    tb_app_take(&tb_j->m_") - 1; const char *tb_c923_1 = ((tb_o_Var *)tb_nn(v_v, 841))->q_name; long long tb_c923_1n = (long long)strlen(tb_c923_1); const char *tb_c923_2 = ", &tb_j->m_rl_"; long long tb_c923_2n = sizeof(", &tb_j->m_rl_") - 1; const char *tb_c923_3 = ((tb_o_Var *)tb_nn(v_v, 841))->q_name; long long tb_c923_3n = (long long)strlen(tb_c923_3); const char *tb_c923_4 = ", &tb_j->m_rc_"; long long tb_c923_4n = sizeof(", &tb_j->m_rc_") - 1; const char *tb_c923_5 = ((tb_o_Var *)tb_nn(v_v, 841))->q_name; long long tb_c923_5n = (long long)strlen(tb_c923_5); const char *tb_c923_6 = ", v_"; long long tb_c923_6n = sizeof(", v_") - 1; const char *tb_c923_7 = ((tb_o_Var *)tb_nn(v_v, 841))->q_name; long long tb_c923_7n = (long long)strlen(tb_c923_7); const char *tb_c923_8 = ", tb_rl_"; long long tb_c923_8n = sizeof(", tb_rl_") - 1; const char *tb_c923_9 = ((tb_o_Var *)tb_nn(v_v, 841))->q_name; long long tb_c923_9n = (long long)strlen(tb_c923_9); const char *tb_c923_10 = ", tb_rc_"; long long tb_c923_10n = sizeof(", tb_rc_") - 1; const char *tb_c923_11 = ((tb_o_Var *)tb_nn(v_v, 841))->q_name; long long tb_c923_11n = (long long)strlen(tb_c923_11); const char *tb_c923_12 = ");\n"; long long tb_c923_12n = sizeof(");\n") - 1; char *tb_c923r = tb_mem(tb_c923_0n + tb_c923_1n + tb_c923_2n + tb_c923_3n + tb_c923_4n + tb_c923_5n + tb_c923_6n + tb_c923_7n + tb_c923_8n + tb_c923_9n + tb_c923_10n + tb_c923_11n + tb_c923_12n + 1), *tb_c923rw = tb_c923r; memcpy(tb_c923rw, tb_c923_0, tb_c923_0n); tb_c923rw += tb_c923_0n; memcpy(tb_c923rw, tb_c923_1, tb_c923_1n); tb_c923rw += tb_c923_1n; memcpy(tb_c923rw, tb_c923_2, tb_c923_2n); tb_c923rw += tb_c923_2n; memcpy(tb_c923rw, tb_c923_3, tb_c923_3n); tb_c923rw += tb_c923_3n; memcpy(tb_c923rw, tb_c923_4, tb_c923_4n); tb_c923rw += tb_c923_4n; memcpy(tb_c923rw, tb_c923_5, tb_c923_5n); tb_c923rw += tb_c923_5n; memcpy(tb_c923rw, tb_c923_6, tb_c923_6n); tb_c923rw += tb_c923_6n; memcpy(tb_c923rw, tb_c923_7, tb_c923_7n); tb_c923rw += tb_c923_7n; memcpy(tb_c923rw, tb_c923_8, tb_c923_8n); tb_c923rw += tb_c923_8n; memcpy(tb_c923rw, tb_c923_9, tb_c923_9n); tb_c923rw += tb_c923_9n; memcpy(tb_c923rw, tb_c923_10, tb_c923_10n); tb_c923rw += tb_c923_10n; memcpy(tb_c923rw, tb_c923_11, tb_c923_11n); tb_c923rw += tb_c923_11n; memcpy(tb_c923rw, tb_c923_12, tb_c923_12n); tb_c923rw += tb_c923_12n; *tb_c923rw = 0; (const char *)tb_c923r; }));
                } else if (((tb_o_Var *)tb_nn(v_v, 842))->q_is_shared) {
                    f_emit__put(v_c, ({ const char *tb_c924_0 = "    *tb_j->p_"; long long tb_c924_0n = sizeof("    *tb_j->p_") - 1; const char *tb_c924_1 = ((tb_o_Var *)tb_nn(v_v, 843))->q_name; long long tb_c924_1n = (long long)strlen(tb_c924_1); const char *tb_c924_2 = " += v_"; long long tb_c924_2n = sizeof(" += v_") - 1; const char *tb_c924_3 = ((tb_o_Var *)tb_nn(v_v, 843))->q_name; long long tb_c924_3n = (long long)strlen(tb_c924_3); const char *tb_c924_4 = ";\n"; long long tb_c924_4n = sizeof(";\n") - 1; char *tb_c924r = tb_mem(tb_c924_0n + tb_c924_1n + tb_c924_2n + tb_c924_3n + tb_c924_4n + 1), *tb_c924rw = tb_c924r; memcpy(tb_c924rw, tb_c924_0, tb_c924_0n); tb_c924rw += tb_c924_0n; memcpy(tb_c924rw, tb_c924_1, tb_c924_1n); tb_c924rw += tb_c924_1n; memcpy(tb_c924rw, tb_c924_2, tb_c924_2n); tb_c924rw += tb_c924_2n; memcpy(tb_c924rw, tb_c924_3, tb_c924_3n); tb_c924rw += tb_c924_3n; memcpy(tb_c924rw, tb_c924_4, tb_c924_4n); tb_c924rw += tb_c924_4n; *tb_c924rw = 0; (const char *)tb_c924r; }));
                }
            }
        }
        f_emit__put(v_c, "    pthread_mutex_unlock(&tb_par_lock);\n");
    }
    f_emit__put(v_c, "    return 0;\n}\n");
    { const char * tb_v925 = tb_join(v_w, ""); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 846))->q_workers) = tb_v925; }
    { tb_list * tb_v926 = v_saved; tb_list * *tb_p926 = &(((tb_o_Comp *)tb_nn(v_c, 847))->q_out); *tb_p926 = tb_v926; }
    const char * v_j = ({ const char *tb_c927_0 = "tb_j"; long long tb_c927_0n = sizeof("tb_j") - 1; const char *tb_c927_1 = v_id; long long tb_c927_1n = (long long)strlen(tb_c927_1); char *tb_c927r = tb_mem(tb_c927_0n + tb_c927_1n + 1), *tb_c927rw = tb_c927r; memcpy(tb_c927rw, tb_c927_0, tb_c927_0n); tb_c927rw += tb_c927_0n; memcpy(tb_c927rw, tb_c927_1, tb_c927_1n); tb_c927rw += tb_c927_1n; *tb_c927rw = 0; (const char *)tb_c927r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c928_0 = "tb_job"; long long tb_c928_0n = sizeof("tb_job") - 1; const char *tb_c928_1 = v_id; long long tb_c928_1n = (long long)strlen(tb_c928_1); const char *tb_c928_2 = " "; long long tb_c928_2n = sizeof(" ") - 1; const char *tb_c928_3 = v_j; long long tb_c928_3n = (long long)strlen(tb_c928_3); const char *tb_c928_4 = " = {0};\n"; long long tb_c928_4n = sizeof(" = {0};\n") - 1; char *tb_c928r = tb_mem(tb_c928_0n + tb_c928_1n + tb_c928_2n + tb_c928_3n + tb_c928_4n + 1), *tb_c928rw = tb_c928r; memcpy(tb_c928rw, tb_c928_0, tb_c928_0n); tb_c928rw += tb_c928_0n; memcpy(tb_c928rw, tb_c928_1, tb_c928_1n); tb_c928rw += tb_c928_1n; memcpy(tb_c928rw, tb_c928_2, tb_c928_2n); tb_c928rw += tb_c928_2n; memcpy(tb_c928rw, tb_c928_3, tb_c928_3n); tb_c928rw += tb_c928_3n; memcpy(tb_c928rw, tb_c928_4, tb_c928_4n); tb_c928rw += tb_c928_4n; *tb_c928rw = 0; (const char *)tb_c928r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    if (v_is_each) {
        f_emit__put(v_c, ({ const char *tb_c929_0 = v_j; long long tb_c929_0n = (long long)strlen(tb_c929_0); const char *tb_c929_1 = ".tb_l = "; long long tb_c929_1n = sizeof(".tb_l = ") - 1; char *tb_c929r = tb_mem(tb_c929_0n + tb_c929_1n + 1), *tb_c929rw = tb_c929r; memcpy(tb_c929rw, tb_c929_0, tb_c929_0n); tb_c929rw += tb_c929_0n; memcpy(tb_c929rw, tb_c929_1, tb_c929_1n); tb_c929rw += tb_c929_1n; *tb_c929rw = 0; (const char *)tb_c929r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 857))->q_kids, 0LL, sizeof(tb_o_Node *), 857)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c930_0 = v_j; long long tb_c930_0n = (long long)strlen(tb_c930_0); const char *tb_c930_1 = ".tb_total = "; long long tb_c930_1n = sizeof(".tb_total = ") - 1; const char *tb_c930_2 = v_j; long long tb_c930_2n = (long long)strlen(tb_c930_2); const char *tb_c930_3 = ".tb_l->len;\n"; long long tb_c930_3n = sizeof(".tb_l->len;\n") - 1; char *tb_c930r = tb_mem(tb_c930_0n + tb_c930_1n + tb_c930_2n + tb_c930_3n + 1), *tb_c930rw = tb_c930r; memcpy(tb_c930rw, tb_c930_0, tb_c930_0n); tb_c930rw += tb_c930_0n; memcpy(tb_c930rw, tb_c930_1, tb_c930_1n); tb_c930rw += tb_c930_1n; memcpy(tb_c930rw, tb_c930_2, tb_c930_2n); tb_c930rw += tb_c930_2n; memcpy(tb_c930rw, tb_c930_3, tb_c930_3n); tb_c930rw += tb_c930_3n; *tb_c930rw = 0; (const char *)tb_c930r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c931_0 = "long long tb_end"; long long tb_c931_0n = sizeof("long long tb_end") - 1; const char *tb_c931_1 = v_id; long long tb_c931_1n = (long long)strlen(tb_c931_1); const char *tb_c931_2 = " = "; long long tb_c931_2n = sizeof(" = ") - 1; char *tb_c931r = tb_mem(tb_c931_0n + tb_c931_1n + tb_c931_2n + 1), *tb_c931rw = tb_c931r; memcpy(tb_c931rw, tb_c931_0, tb_c931_0n); tb_c931rw += tb_c931_0n; memcpy(tb_c931rw, tb_c931_1, tb_c931_1n); tb_c931rw += tb_c931_1n; memcpy(tb_c931rw, tb_c931_2, tb_c931_2n); tb_c931rw += tb_c931_2n; *tb_c931rw = 0; (const char *)tb_c931r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 863))->q_kids, 1LL, sizeof(tb_o_Node *), 863)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c932_0 = v_j; long long tb_c932_0n = (long long)strlen(tb_c932_0); const char *tb_c932_1 = ".tb_lo = "; long long tb_c932_1n = sizeof(".tb_lo = ") - 1; char *tb_c932r = tb_mem(tb_c932_0n + tb_c932_1n + 1), *tb_c932rw = tb_c932r; memcpy(tb_c932rw, tb_c932_0, tb_c932_0n); tb_c932rw += tb_c932_0n; memcpy(tb_c932rw, tb_c932_1, tb_c932_1n); tb_c932rw += tb_c932_1n; *tb_c932rw = 0; (const char *)tb_c932r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 867))->q_kids, 0LL, sizeof(tb_o_Node *), 867)));
        f_emit__put(v_c, ";\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c933_0 = v_j; long long tb_c933_0n = (long long)strlen(tb_c933_0); const char *tb_c933_1 = ".tb_total = tb_end"; long long tb_c933_1n = sizeof(".tb_total = tb_end") - 1; const char *tb_c933_2 = v_id; long long tb_c933_2n = (long long)strlen(tb_c933_2); const char *tb_c933_3 = " < "; long long tb_c933_3n = sizeof(" < ") - 1; const char *tb_c933_4 = v_j; long long tb_c933_4n = (long long)strlen(tb_c933_4); const char *tb_c933_5 = ".tb_lo ? 0 : tb_end"; long long tb_c933_5n = sizeof(".tb_lo ? 0 : tb_end") - 1; const char *tb_c933_6 = v_id; long long tb_c933_6n = (long long)strlen(tb_c933_6); const char *tb_c933_7 = " - "; long long tb_c933_7n = sizeof(" - ") - 1; const char *tb_c933_8 = v_j; long long tb_c933_8n = (long long)strlen(tb_c933_8); const char *tb_c933_9 = ".tb_lo + 1;\n"; long long tb_c933_9n = sizeof(".tb_lo + 1;\n") - 1; char *tb_c933r = tb_mem(tb_c933_0n + tb_c933_1n + tb_c933_2n + tb_c933_3n + tb_c933_4n + tb_c933_5n + tb_c933_6n + tb_c933_7n + tb_c933_8n + tb_c933_9n + 1), *tb_c933rw = tb_c933r; memcpy(tb_c933rw, tb_c933_0, tb_c933_0n); tb_c933rw += tb_c933_0n; memcpy(tb_c933rw, tb_c933_1, tb_c933_1n); tb_c933rw += tb_c933_1n; memcpy(tb_c933rw, tb_c933_2, tb_c933_2n); tb_c933rw += tb_c933_2n; memcpy(tb_c933rw, tb_c933_3, tb_c933_3n); tb_c933rw += tb_c933_3n; memcpy(tb_c933rw, tb_c933_4, tb_c933_4n); tb_c933rw += tb_c933_4n; memcpy(tb_c933rw, tb_c933_5, tb_c933_5n); tb_c933rw += tb_c933_5n; memcpy(tb_c933rw, tb_c933_6, tb_c933_6n); tb_c933rw += tb_c933_6n; memcpy(tb_c933rw, tb_c933_7, tb_c933_7n); tb_c933rw += tb_c933_7n; memcpy(tb_c933rw, tb_c933_8, tb_c933_8n); tb_c933rw += tb_c933_8n; memcpy(tb_c933rw, tb_c933_9, tb_c933_9n); tb_c933rw += tb_c933_9n; *tb_c933rw = 0; (const char *)tb_c933r; }));
    }
    {
        tb_list * tb_l934 = ((tb_o_Node *)tb_nn(v_n, 871))->q_caps;
        for (long long tb_i934 = 0; tb_i934 < tb_l934->len; tb_i934++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l934->data)[tb_i934];
            f_emit__indent(v_c, (v_depth + 1LL));
            if ((((tb_o_Var *)tb_nn(v_v, 873))->q_is_shared && ({ const char *tb_s935 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 873))->q_type); ((unsigned char)tb_s935[0] == 116 && (unsigned char)tb_s935[1] == 101 && (unsigned char)tb_s935[2] == 120 && (unsigned char)tb_s935[3] == 116 && tb_s935[4] == 0); }))) {
                f_emit__put(v_c, ({ const char *tb_c936_0 = v_j; long long tb_c936_0n = (long long)strlen(tb_c936_0); const char *tb_c936_1 = ".m_"; long long tb_c936_1n = sizeof(".m_") - 1; const char *tb_c936_2 = ((tb_o_Var *)tb_nn(v_v, 874))->q_name; long long tb_c936_2n = (long long)strlen(tb_c936_2); const char *tb_c936_3 = " = v_"; long long tb_c936_3n = sizeof(" = v_") - 1; const char *tb_c936_4 = ((tb_o_Var *)tb_nn(v_v, 874))->q_name; long long tb_c936_4n = (long long)strlen(tb_c936_4); const char *tb_c936_5 = "; "; long long tb_c936_5n = sizeof("; ") - 1; const char *tb_c936_6 = v_j; long long tb_c936_6n = (long long)strlen(tb_c936_6); const char *tb_c936_7 = ".m_rl_"; long long tb_c936_7n = sizeof(".m_rl_") - 1; const char *tb_c936_8 = ((tb_o_Var *)tb_nn(v_v, 874))->q_name; long long tb_c936_8n = (long long)strlen(tb_c936_8); const char *tb_c936_9 = " = -1;\n"; long long tb_c936_9n = sizeof(" = -1;\n") - 1; char *tb_c936r = tb_mem(tb_c936_0n + tb_c936_1n + tb_c936_2n + tb_c936_3n + tb_c936_4n + tb_c936_5n + tb_c936_6n + tb_c936_7n + tb_c936_8n + tb_c936_9n + 1), *tb_c936rw = tb_c936r; memcpy(tb_c936rw, tb_c936_0, tb_c936_0n); tb_c936rw += tb_c936_0n; memcpy(tb_c936rw, tb_c936_1, tb_c936_1n); tb_c936rw += tb_c936_1n; memcpy(tb_c936rw, tb_c936_2, tb_c936_2n); tb_c936rw += tb_c936_2n; memcpy(tb_c936rw, tb_c936_3, tb_c936_3n); tb_c936rw += tb_c936_3n; memcpy(tb_c936rw, tb_c936_4, tb_c936_4n); tb_c936rw += tb_c936_4n; memcpy(tb_c936rw, tb_c936_5, tb_c936_5n); tb_c936rw += tb_c936_5n; memcpy(tb_c936rw, tb_c936_6, tb_c936_6n); tb_c936rw += tb_c936_6n; memcpy(tb_c936rw, tb_c936_7, tb_c936_7n); tb_c936rw += tb_c936_7n; memcpy(tb_c936rw, tb_c936_8, tb_c936_8n); tb_c936rw += tb_c936_8n; memcpy(tb_c936rw, tb_c936_9, tb_c936_9n); tb_c936rw += tb_c936_9n; *tb_c936rw = 0; (const char *)tb_c936r; }));
            } else if (((tb_o_Var *)tb_nn(v_v, 875))->q_is_shared) {
                f_emit__put(v_c, ({ const char *tb_c937_0 = v_j; long long tb_c937_0n = (long long)strlen(tb_c937_0); const char *tb_c937_1 = ".p_"; long long tb_c937_1n = sizeof(".p_") - 1; const char *tb_c937_2 = ((tb_o_Var *)tb_nn(v_v, 876))->q_name; long long tb_c937_2n = (long long)strlen(tb_c937_2); const char *tb_c937_3 = " = &v_"; long long tb_c937_3n = sizeof(" = &v_") - 1; const char *tb_c937_4 = ((tb_o_Var *)tb_nn(v_v, 876))->q_name; long long tb_c937_4n = (long long)strlen(tb_c937_4); const char *tb_c937_5 = ";\n"; long long tb_c937_5n = sizeof(";\n") - 1; char *tb_c937r = tb_mem(tb_c937_0n + tb_c937_1n + tb_c937_2n + tb_c937_3n + tb_c937_4n + tb_c937_5n + 1), *tb_c937rw = tb_c937r; memcpy(tb_c937rw, tb_c937_0, tb_c937_0n); tb_c937rw += tb_c937_0n; memcpy(tb_c937rw, tb_c937_1, tb_c937_1n); tb_c937rw += tb_c937_1n; memcpy(tb_c937rw, tb_c937_2, tb_c937_2n); tb_c937rw += tb_c937_2n; memcpy(tb_c937rw, tb_c937_3, tb_c937_3n); tb_c937rw += tb_c937_3n; memcpy(tb_c937rw, tb_c937_4, tb_c937_4n); tb_c937rw += tb_c937_4n; memcpy(tb_c937rw, tb_c937_5, tb_c937_5n); tb_c937rw += tb_c937_5n; *tb_c937rw = 0; (const char *)tb_c937r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c938_0 = v_j; long long tb_c938_0n = (long long)strlen(tb_c938_0); const char *tb_c938_1 = ".v_"; long long tb_c938_1n = sizeof(".v_") - 1; const char *tb_c938_2 = ((tb_o_Var *)tb_nn(v_v, 878))->q_name; long long tb_c938_2n = (long long)strlen(tb_c938_2); const char *tb_c938_3 = " = v_"; long long tb_c938_3n = sizeof(" = v_") - 1; const char *tb_c938_4 = ((tb_o_Var *)tb_nn(v_v, 878))->q_name; long long tb_c938_4n = (long long)strlen(tb_c938_4); const char *tb_c938_5 = ";\n"; long long tb_c938_5n = sizeof(";\n") - 1; char *tb_c938r = tb_mem(tb_c938_0n + tb_c938_1n + tb_c938_2n + tb_c938_3n + tb_c938_4n + tb_c938_5n + 1), *tb_c938rw = tb_c938r; memcpy(tb_c938rw, tb_c938_0, tb_c938_0n); tb_c938rw += tb_c938_0n; memcpy(tb_c938rw, tb_c938_1, tb_c938_1n); tb_c938rw += tb_c938_1n; memcpy(tb_c938rw, tb_c938_2, tb_c938_2n); tb_c938rw += tb_c938_2n; memcpy(tb_c938rw, tb_c938_3, tb_c938_3n); tb_c938rw += tb_c938_3n; memcpy(tb_c938rw, tb_c938_4, tb_c938_4n); tb_c938rw += tb_c938_4n; memcpy(tb_c938rw, tb_c938_5, tb_c938_5n); tb_c938rw += tb_c938_5n; *tb_c938rw = 0; (const char *)tb_c938r; }));
            }
        }
    }
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c939_0 = v_j; long long tb_c939_0n = (long long)strlen(tb_c939_0); const char *tb_c939_1 = ".tb_step = tb_par_step("; long long tb_c939_1n = sizeof(".tb_step = tb_par_step(") - 1; const char *tb_c939_2 = v_j; long long tb_c939_2n = (long long)strlen(tb_c939_2); const char *tb_c939_3 = ".tb_total);\n"; long long tb_c939_3n = sizeof(".tb_total);\n") - 1; char *tb_c939r = tb_mem(tb_c939_0n + tb_c939_1n + tb_c939_2n + tb_c939_3n + 1), *tb_c939rw = tb_c939r; memcpy(tb_c939rw, tb_c939_0, tb_c939_0n); tb_c939rw += tb_c939_0n; memcpy(tb_c939rw, tb_c939_1, tb_c939_1n); tb_c939rw += tb_c939_1n; memcpy(tb_c939rw, tb_c939_2, tb_c939_2n); tb_c939rw += tb_c939_2n; memcpy(tb_c939rw, tb_c939_3, tb_c939_3n); tb_c939rw += tb_c939_3n; *tb_c939rw = 0; (const char *)tb_c939r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c940_0 = "tb_par_run(tb_work"; long long tb_c940_0n = sizeof("tb_par_run(tb_work") - 1; const char *tb_c940_1 = v_id; long long tb_c940_1n = (long long)strlen(tb_c940_1); const char *tb_c940_2 = ", &"; long long tb_c940_2n = sizeof(", &") - 1; const char *tb_c940_3 = v_j; long long tb_c940_3n = (long long)strlen(tb_c940_3); const char *tb_c940_4 = ", "; long long tb_c940_4n = sizeof(", ") - 1; const char *tb_c940_5 = v_j; long long tb_c940_5n = (long long)strlen(tb_c940_5); const char *tb_c940_6 = ".tb_total);\n"; long long tb_c940_6n = sizeof(".tb_total);\n") - 1; char *tb_c940r = tb_mem(tb_c940_0n + tb_c940_1n + tb_c940_2n + tb_c940_3n + tb_c940_4n + tb_c940_5n + tb_c940_6n + 1), *tb_c940rw = tb_c940r; memcpy(tb_c940rw, tb_c940_0, tb_c940_0n); tb_c940rw += tb_c940_0n; memcpy(tb_c940rw, tb_c940_1, tb_c940_1n); tb_c940rw += tb_c940_1n; memcpy(tb_c940rw, tb_c940_2, tb_c940_2n); tb_c940rw += tb_c940_2n; memcpy(tb_c940rw, tb_c940_3, tb_c940_3n); tb_c940rw += tb_c940_3n; memcpy(tb_c940rw, tb_c940_4, tb_c940_4n); tb_c940rw += tb_c940_4n; memcpy(tb_c940rw, tb_c940_5, tb_c940_5n); tb_c940rw += tb_c940_5n; memcpy(tb_c940rw, tb_c940_6, tb_c940_6n); tb_c940rw += tb_c940_6n; *tb_c940rw = 0; (const char *)tb_c940r; }));
    {
        tb_list * tb_l941 = ((tb_o_Node *)tb_nn(v_n, 883))->q_caps;
        for (long long tb_i941 = 0; tb_i941 < tb_l941->len; tb_i941++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l941->data)[tb_i941];
            if ((((tb_o_Var *)tb_nn(v_v, 884))->q_is_shared && ({ const char *tb_s942 = f_check__kind_of(v_c, ((tb_o_Var *)tb_nn(v_v, 884))->q_type); ((unsigned char)tb_s942[0] == 116 && (unsigned char)tb_s942[1] == 101 && (unsigned char)tb_s942[2] == 120 && (unsigned char)tb_s942[3] == 116 && tb_s942[4] == 0); }))) {
                f_emit__indent(v_c, (v_depth + 1LL));
                f_emit__put(v_c, ({ const char *tb_c943_0 = "v_"; long long tb_c943_0n = sizeof("v_") - 1; const char *tb_c943_1 = ((tb_o_Var *)tb_nn(v_v, 886))->q_name; long long tb_c943_1n = (long long)strlen(tb_c943_1); const char *tb_c943_2 = " = "; long long tb_c943_2n = sizeof(" = ") - 1; const char *tb_c943_3 = v_j; long long tb_c943_3n = (long long)strlen(tb_c943_3); const char *tb_c943_4 = ".m_"; long long tb_c943_4n = sizeof(".m_") - 1; const char *tb_c943_5 = ((tb_o_Var *)tb_nn(v_v, 886))->q_name; long long tb_c943_5n = (long long)strlen(tb_c943_5); const char *tb_c943_6 = ";\n"; long long tb_c943_6n = sizeof(";\n") - 1; char *tb_c943r = tb_mem(tb_c943_0n + tb_c943_1n + tb_c943_2n + tb_c943_3n + tb_c943_4n + tb_c943_5n + tb_c943_6n + 1), *tb_c943rw = tb_c943r; memcpy(tb_c943rw, tb_c943_0, tb_c943_0n); tb_c943rw += tb_c943_0n; memcpy(tb_c943rw, tb_c943_1, tb_c943_1n); tb_c943rw += tb_c943_1n; memcpy(tb_c943rw, tb_c943_2, tb_c943_2n); tb_c943rw += tb_c943_2n; memcpy(tb_c943rw, tb_c943_3, tb_c943_3n); tb_c943rw += tb_c943_3n; memcpy(tb_c943rw, tb_c943_4, tb_c943_4n); tb_c943rw += tb_c943_4n; memcpy(tb_c943rw, tb_c943_5, tb_c943_5n); tb_c943rw += tb_c943_5n; memcpy(tb_c943rw, tb_c943_6, tb_c943_6n); tb_c943rw += tb_c943_6n; *tb_c943rw = 0; (const char *)tb_c943r; }));
            }
        }
    }
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static long long f_emit__vec_index(tb_o_Comp * v_c, const char * v_name) {
    {
        long long tb_end944 = ((((tb_o_Comp *)tb_nn(v_c, 891))->q_vec_names)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end944; v_i++) {
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
    { long long tb_v945 = 1LL; long long *tb_p945 = &(((tb_o_Comp *)tb_nn(v_c, 899))->q_label); *tb_p945 += tb_v945; }
    const char * v_id = ({ const char *tb_c946_0 = ""; long long tb_c946_0n = sizeof("") - 1; char tb_c946_1d[24]; long long tb_c946_1n; const char *tb_c946_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 900))->q_label, tb_c946_1d, &tb_c946_1n); char *tb_c946r = tb_mem(tb_c946_0n + tb_c946_1n + 1), *tb_c946rw = tb_c946r; memcpy(tb_c946rw, tb_c946_0, tb_c946_0n); tb_c946rw += tb_c946_0n; memcpy(tb_c946rw, tb_c946_1, tb_c946_1n); tb_c946rw += tb_c946_1n; *tb_c946rw = 0; (const char *)tb_c946r; });
    const char * v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 901))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 901))->q_kids, 0LL, sizeof(tb_o_Node *), 901)), 901))->q_ty, sizeof(tb_o_Type *), 901)), 901))->q_elem);
    long long v_d = (v_depth + 1LL);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c947_0 = "tb_list *tb_l"; long long tb_c947_0n = sizeof("tb_list *tb_l") - 1; const char *tb_c947_1 = v_id; long long tb_c947_1n = (long long)strlen(tb_c947_1); const char *tb_c947_2 = " = "; long long tb_c947_2n = sizeof(" = ") - 1; char *tb_c947r = tb_mem(tb_c947_0n + tb_c947_1n + tb_c947_2n + 1), *tb_c947rw = tb_c947r; memcpy(tb_c947rw, tb_c947_0, tb_c947_0n); tb_c947rw += tb_c947_0n; memcpy(tb_c947rw, tb_c947_1, tb_c947_1n); tb_c947rw += tb_c947_1n; memcpy(tb_c947rw, tb_c947_2, tb_c947_2n); tb_c947rw += tb_c947_2n; *tb_c947rw = 0; (const char *)tb_c947r; }));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 907))->q_kids, 0LL, sizeof(tb_o_Node *), 907)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c948_0 = v_elem; long long tb_c948_0n = (long long)strlen(tb_c948_0); const char *tb_c948_1 = " *tb_d"; long long tb_c948_1n = sizeof(" *tb_d") - 1; const char *tb_c948_2 = v_id; long long tb_c948_2n = (long long)strlen(tb_c948_2); const char *tb_c948_3 = " = ("; long long tb_c948_3n = sizeof(" = (") - 1; const char *tb_c948_4 = v_elem; long long tb_c948_4n = (long long)strlen(tb_c948_4); const char *tb_c948_5 = " *)tb_l"; long long tb_c948_5n = sizeof(" *)tb_l") - 1; const char *tb_c948_6 = v_id; long long tb_c948_6n = (long long)strlen(tb_c948_6); const char *tb_c948_7 = "->data;\n"; long long tb_c948_7n = sizeof("->data;\n") - 1; char *tb_c948r = tb_mem(tb_c948_0n + tb_c948_1n + tb_c948_2n + tb_c948_3n + tb_c948_4n + tb_c948_5n + tb_c948_6n + tb_c948_7n + 1), *tb_c948rw = tb_c948r; memcpy(tb_c948rw, tb_c948_0, tb_c948_0n); tb_c948rw += tb_c948_0n; memcpy(tb_c948rw, tb_c948_1, tb_c948_1n); tb_c948rw += tb_c948_1n; memcpy(tb_c948rw, tb_c948_2, tb_c948_2n); tb_c948rw += tb_c948_2n; memcpy(tb_c948rw, tb_c948_3, tb_c948_3n); tb_c948rw += tb_c948_3n; memcpy(tb_c948rw, tb_c948_4, tb_c948_4n); tb_c948rw += tb_c948_4n; memcpy(tb_c948rw, tb_c948_5, tb_c948_5n); tb_c948rw += tb_c948_5n; memcpy(tb_c948rw, tb_c948_6, tb_c948_6n); tb_c948rw += tb_c948_6n; memcpy(tb_c948rw, tb_c948_7, tb_c948_7n); tb_c948rw += tb_c948_7n; *tb_c948rw = 0; (const char *)tb_c948r; }));
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c949_0 = "long long tb_n"; long long tb_c949_0n = sizeof("long long tb_n") - 1; const char *tb_c949_1 = v_id; long long tb_c949_1n = (long long)strlen(tb_c949_1); const char *tb_c949_2 = " = tb_l"; long long tb_c949_2n = sizeof(" = tb_l") - 1; const char *tb_c949_3 = v_id; long long tb_c949_3n = (long long)strlen(tb_c949_3); const char *tb_c949_4 = "->len, tb_i"; long long tb_c949_4n = sizeof("->len, tb_i") - 1; const char *tb_c949_5 = v_id; long long tb_c949_5n = (long long)strlen(tb_c949_5); const char *tb_c949_6 = " = 0;\n"; long long tb_c949_6n = sizeof(" = 0;\n") - 1; char *tb_c949r = tb_mem(tb_c949_0n + tb_c949_1n + tb_c949_2n + tb_c949_3n + tb_c949_4n + tb_c949_5n + tb_c949_6n + 1), *tb_c949rw = tb_c949r; memcpy(tb_c949rw, tb_c949_0, tb_c949_0n); tb_c949rw += tb_c949_0n; memcpy(tb_c949rw, tb_c949_1, tb_c949_1n); tb_c949rw += tb_c949_1n; memcpy(tb_c949rw, tb_c949_2, tb_c949_2n); tb_c949rw += tb_c949_2n; memcpy(tb_c949rw, tb_c949_3, tb_c949_3n); tb_c949rw += tb_c949_3n; memcpy(tb_c949rw, tb_c949_4, tb_c949_4n); tb_c949rw += tb_c949_4n; memcpy(tb_c949rw, tb_c949_5, tb_c949_5n); tb_c949rw += tb_c949_5n; memcpy(tb_c949rw, tb_c949_6, tb_c949_6n); tb_c949rw += tb_c949_6n; *tb_c949rw = 0; (const char *)tb_c949r; }));
    {
        tb_list * tb_l950 = ((tb_o_Node *)tb_nn(v_n, 915))->q_caps;
        for (long long tb_i950 = 0; tb_i950 < tb_l950->len; tb_i950++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l950->data)[tb_i950];
            if (((tb_o_Var *)tb_nn(v_v, 916))->q_is_shared) {
                f_emit__indent(v_c, v_d);
                f_emit__put(v_c, ({ const char *tb_c951_0 = f_emit__c_type(v_c, ((tb_o_Var *)tb_nn(v_v, 918))->q_type); long long tb_c951_0n = (long long)strlen(tb_c951_0); const char *tb_c951_1 = " "; long long tb_c951_1n = sizeof(" ") - 1; char *tb_c951r = tb_mem(tb_c951_0n + tb_c951_1n + 1), *tb_c951rw = tb_c951r; memcpy(tb_c951rw, tb_c951_0, tb_c951_0n); tb_c951rw += tb_c951_0n; memcpy(tb_c951rw, tb_c951_1, tb_c951_1n); tb_c951rw += tb_c951_1n; *tb_c951rw = 0; (const char *)tb_c951r; }));
                {
                    long long tb_end952 = 7LL;
                    for (long long v_k = 0LL; v_k <= tb_end952; v_k++) {
                        if ((v_k > 0LL)) {
                            f_emit__put(v_c, ", ");
                        }
                        f_emit__put(v_c, ({ const char *tb_c953_0 = "tb_a"; long long tb_c953_0n = sizeof("tb_a") - 1; const char *tb_c953_1 = v_id; long long tb_c953_1n = (long long)strlen(tb_c953_1); const char *tb_c953_2 = "_"; long long tb_c953_2n = sizeof("_") - 1; const char *tb_c953_3 = ((tb_o_Var *)tb_nn(v_v, 922))->q_name; long long tb_c953_3n = (long long)strlen(tb_c953_3); const char *tb_c953_4 = "_"; long long tb_c953_4n = sizeof("_") - 1; char tb_c953_5d[24]; long long tb_c953_5n; const char *tb_c953_5 = tb_digits(v_k, tb_c953_5d, &tb_c953_5n); const char *tb_c953_6 = " = 0"; long long tb_c953_6n = sizeof(" = 0") - 1; char *tb_c953r = tb_mem(tb_c953_0n + tb_c953_1n + tb_c953_2n + tb_c953_3n + tb_c953_4n + tb_c953_5n + tb_c953_6n + 1), *tb_c953rw = tb_c953r; memcpy(tb_c953rw, tb_c953_0, tb_c953_0n); tb_c953rw += tb_c953_0n; memcpy(tb_c953rw, tb_c953_1, tb_c953_1n); tb_c953rw += tb_c953_1n; memcpy(tb_c953rw, tb_c953_2, tb_c953_2n); tb_c953rw += tb_c953_2n; memcpy(tb_c953rw, tb_c953_3, tb_c953_3n); tb_c953rw += tb_c953_3n; memcpy(tb_c953rw, tb_c953_4, tb_c953_4n); tb_c953rw += tb_c953_4n; memcpy(tb_c953rw, tb_c953_5, tb_c953_5n); tb_c953rw += tb_c953_5n; memcpy(tb_c953rw, tb_c953_6, tb_c953_6n); tb_c953rw += tb_c953_6n; *tb_c953rw = 0; (const char *)tb_c953r; }));
                    }
                }
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c954_0 = "for (; tb_i"; long long tb_c954_0n = sizeof("for (; tb_i") - 1; const char *tb_c954_1 = v_id; long long tb_c954_1n = (long long)strlen(tb_c954_1); const char *tb_c954_2 = " + 8 <= tb_n"; long long tb_c954_2n = sizeof(" + 8 <= tb_n") - 1; const char *tb_c954_3 = v_id; long long tb_c954_3n = (long long)strlen(tb_c954_3); const char *tb_c954_4 = "; tb_i"; long long tb_c954_4n = sizeof("; tb_i") - 1; const char *tb_c954_5 = v_id; long long tb_c954_5n = (long long)strlen(tb_c954_5); const char *tb_c954_6 = " += 8) {\n"; long long tb_c954_6n = sizeof(" += 8) {\n") - 1; char *tb_c954r = tb_mem(tb_c954_0n + tb_c954_1n + tb_c954_2n + tb_c954_3n + tb_c954_4n + tb_c954_5n + tb_c954_6n + 1), *tb_c954rw = tb_c954r; memcpy(tb_c954rw, tb_c954_0, tb_c954_0n); tb_c954rw += tb_c954_0n; memcpy(tb_c954rw, tb_c954_1, tb_c954_1n); tb_c954rw += tb_c954_1n; memcpy(tb_c954rw, tb_c954_2, tb_c954_2n); tb_c954rw += tb_c954_2n; memcpy(tb_c954rw, tb_c954_3, tb_c954_3n); tb_c954rw += tb_c954_3n; memcpy(tb_c954rw, tb_c954_4, tb_c954_4n); tb_c954rw += tb_c954_4n; memcpy(tb_c954rw, tb_c954_5, tb_c954_5n); tb_c954rw += tb_c954_5n; memcpy(tb_c954rw, tb_c954_6, tb_c954_6n); tb_c954rw += tb_c954_6n; *tb_c954rw = 0; (const char *)tb_c954r; }));
    {
        long long tb_end955 = 7LL;
        for (long long v_k = 0LL; v_k <= tb_end955; v_k++) {
            f_emit__indent(v_c, (v_d + 1LL));
            f_emit__put(v_c, "do {\n");
            f_emit__indent(v_c, (v_d + 2LL));
            f_emit__put(v_c, ({ const char *tb_c956_0 = v_elem; long long tb_c956_0n = (long long)strlen(tb_c956_0); const char *tb_c956_1 = " const v_"; long long tb_c956_1n = sizeof(" const v_") - 1; const char *tb_c956_2 = ((tb_o_Node *)tb_nn(v_n, 932))->q_name; long long tb_c956_2n = (long long)strlen(tb_c956_2); const char *tb_c956_3 = " = tb_d"; long long tb_c956_3n = sizeof(" = tb_d") - 1; const char *tb_c956_4 = v_id; long long tb_c956_4n = (long long)strlen(tb_c956_4); const char *tb_c956_5 = "[tb_i"; long long tb_c956_5n = sizeof("[tb_i") - 1; const char *tb_c956_6 = v_id; long long tb_c956_6n = (long long)strlen(tb_c956_6); const char *tb_c956_7 = " + "; long long tb_c956_7n = sizeof(" + ") - 1; char tb_c956_8d[24]; long long tb_c956_8n; const char *tb_c956_8 = tb_digits(v_k, tb_c956_8d, &tb_c956_8n); const char *tb_c956_9 = "];\n"; long long tb_c956_9n = sizeof("];\n") - 1; char *tb_c956r = tb_mem(tb_c956_0n + tb_c956_1n + tb_c956_2n + tb_c956_3n + tb_c956_4n + tb_c956_5n + tb_c956_6n + tb_c956_7n + tb_c956_8n + tb_c956_9n + 1), *tb_c956rw = tb_c956r; memcpy(tb_c956rw, tb_c956_0, tb_c956_0n); tb_c956rw += tb_c956_0n; memcpy(tb_c956rw, tb_c956_1, tb_c956_1n); tb_c956rw += tb_c956_1n; memcpy(tb_c956rw, tb_c956_2, tb_c956_2n); tb_c956rw += tb_c956_2n; memcpy(tb_c956rw, tb_c956_3, tb_c956_3n); tb_c956rw += tb_c956_3n; memcpy(tb_c956rw, tb_c956_4, tb_c956_4n); tb_c956rw += tb_c956_4n; memcpy(tb_c956rw, tb_c956_5, tb_c956_5n); tb_c956rw += tb_c956_5n; memcpy(tb_c956rw, tb_c956_6, tb_c956_6n); tb_c956rw += tb_c956_6n; memcpy(tb_c956rw, tb_c956_7, tb_c956_7n); tb_c956rw += tb_c956_7n; memcpy(tb_c956rw, tb_c956_8, tb_c956_8n); tb_c956rw += tb_c956_8n; memcpy(tb_c956rw, tb_c956_9, tb_c956_9n); tb_c956rw += tb_c956_9n; *tb_c956rw = 0; (const char *)tb_c956r; }));
            {
                tb_list * tb_l957 = ((tb_o_Node *)tb_nn(v_n, 933))->q_caps;
                for (long long tb_i957 = 0; tb_i957 < tb_l957->len; tb_i957++) {
                    tb_o_Var * const v_v = ((tb_o_Var * *)tb_l957->data)[tb_i957];
                    if (((tb_o_Var *)tb_nn(v_v, 934))->q_is_shared) {
                        { const char * tb_v958 = ((tb_o_Var *)tb_nn(v_v, 935))->q_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 935))->q_vec_names) = tb_v958; }
                        { const char * tb_v959 = ({ const char *tb_c960_0 = "tb_a"; long long tb_c960_0n = sizeof("tb_a") - 1; const char *tb_c960_1 = v_id; long long tb_c960_1n = (long long)strlen(tb_c960_1); const char *tb_c960_2 = "_"; long long tb_c960_2n = sizeof("_") - 1; const char *tb_c960_3 = ((tb_o_Var *)tb_nn(v_v, 936))->q_name; long long tb_c960_3n = (long long)strlen(tb_c960_3); const char *tb_c960_4 = "_"; long long tb_c960_4n = sizeof("_") - 1; char tb_c960_5d[24]; long long tb_c960_5n; const char *tb_c960_5 = tb_digits(v_k, tb_c960_5d, &tb_c960_5n); char *tb_c960r = tb_mem(tb_c960_0n + tb_c960_1n + tb_c960_2n + tb_c960_3n + tb_c960_4n + tb_c960_5n + 1), *tb_c960rw = tb_c960r; memcpy(tb_c960rw, tb_c960_0, tb_c960_0n); tb_c960rw += tb_c960_0n; memcpy(tb_c960rw, tb_c960_1, tb_c960_1n); tb_c960rw += tb_c960_1n; memcpy(tb_c960rw, tb_c960_2, tb_c960_2n); tb_c960rw += tb_c960_2n; memcpy(tb_c960rw, tb_c960_3, tb_c960_3n); tb_c960rw += tb_c960_3n; memcpy(tb_c960rw, tb_c960_4, tb_c960_4n); tb_c960rw += tb_c960_4n; memcpy(tb_c960rw, tb_c960_5, tb_c960_5n); tb_c960rw += tb_c960_5n; *tb_c960rw = 0; (const char *)tb_c960r; }); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 936))->q_vec_repl) = tb_v959; }
                    }
                }
            }
            f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 937))->q_kids, 1LL, sizeof(tb_o_Node *), 937)), (v_d + 2LL));
            tb_list * v_fresh_names = tb_list_new(sizeof(const char *));
            tb_list * v_fresh_repl = tb_list_new(sizeof(const char *));
            { tb_list * tb_v961 = v_fresh_names; tb_list * *tb_p961 = &(((tb_o_Comp *)tb_nn(v_c, 940))->q_vec_names); *tb_p961 = tb_v961; }
            { tb_list * tb_v962 = v_fresh_repl; tb_list * *tb_p962 = &(((tb_o_Comp *)tb_nn(v_c, 941))->q_vec_repl); *tb_p962 = tb_v962; }
            f_emit__indent(v_c, (v_d + 1LL));
            f_emit__put(v_c, "} while (0);\n");
        }
    }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, ({ const char *tb_c963_0 = "for (; tb_i"; long long tb_c963_0n = sizeof("for (; tb_i") - 1; const char *tb_c963_1 = v_id; long long tb_c963_1n = (long long)strlen(tb_c963_1); const char *tb_c963_2 = " < tb_n"; long long tb_c963_2n = sizeof(" < tb_n") - 1; const char *tb_c963_3 = v_id; long long tb_c963_3n = (long long)strlen(tb_c963_3); const char *tb_c963_4 = "; tb_i"; long long tb_c963_4n = sizeof("; tb_i") - 1; const char *tb_c963_5 = v_id; long long tb_c963_5n = (long long)strlen(tb_c963_5); const char *tb_c963_6 = "++) {\n"; long long tb_c963_6n = sizeof("++) {\n") - 1; char *tb_c963r = tb_mem(tb_c963_0n + tb_c963_1n + tb_c963_2n + tb_c963_3n + tb_c963_4n + tb_c963_5n + tb_c963_6n + 1), *tb_c963rw = tb_c963r; memcpy(tb_c963rw, tb_c963_0, tb_c963_0n); tb_c963rw += tb_c963_0n; memcpy(tb_c963rw, tb_c963_1, tb_c963_1n); tb_c963rw += tb_c963_1n; memcpy(tb_c963rw, tb_c963_2, tb_c963_2n); tb_c963rw += tb_c963_2n; memcpy(tb_c963rw, tb_c963_3, tb_c963_3n); tb_c963rw += tb_c963_3n; memcpy(tb_c963rw, tb_c963_4, tb_c963_4n); tb_c963rw += tb_c963_4n; memcpy(tb_c963rw, tb_c963_5, tb_c963_5n); tb_c963rw += tb_c963_5n; memcpy(tb_c963rw, tb_c963_6, tb_c963_6n); tb_c963rw += tb_c963_6n; *tb_c963rw = 0; (const char *)tb_c963r; }));
    f_emit__indent(v_c, (v_d + 1LL));
    f_emit__put(v_c, ({ const char *tb_c964_0 = v_elem; long long tb_c964_0n = (long long)strlen(tb_c964_0); const char *tb_c964_1 = " const v_"; long long tb_c964_1n = sizeof(" const v_") - 1; const char *tb_c964_2 = ((tb_o_Node *)tb_nn(v_n, 950))->q_name; long long tb_c964_2n = (long long)strlen(tb_c964_2); const char *tb_c964_3 = " = tb_d"; long long tb_c964_3n = sizeof(" = tb_d") - 1; const char *tb_c964_4 = v_id; long long tb_c964_4n = (long long)strlen(tb_c964_4); const char *tb_c964_5 = "[tb_i"; long long tb_c964_5n = sizeof("[tb_i") - 1; const char *tb_c964_6 = v_id; long long tb_c964_6n = (long long)strlen(tb_c964_6); const char *tb_c964_7 = "];\n"; long long tb_c964_7n = sizeof("];\n") - 1; char *tb_c964r = tb_mem(tb_c964_0n + tb_c964_1n + tb_c964_2n + tb_c964_3n + tb_c964_4n + tb_c964_5n + tb_c964_6n + tb_c964_7n + 1), *tb_c964rw = tb_c964r; memcpy(tb_c964rw, tb_c964_0, tb_c964_0n); tb_c964rw += tb_c964_0n; memcpy(tb_c964rw, tb_c964_1, tb_c964_1n); tb_c964rw += tb_c964_1n; memcpy(tb_c964rw, tb_c964_2, tb_c964_2n); tb_c964rw += tb_c964_2n; memcpy(tb_c964rw, tb_c964_3, tb_c964_3n); tb_c964rw += tb_c964_3n; memcpy(tb_c964rw, tb_c964_4, tb_c964_4n); tb_c964rw += tb_c964_4n; memcpy(tb_c964rw, tb_c964_5, tb_c964_5n); tb_c964rw += tb_c964_5n; memcpy(tb_c964rw, tb_c964_6, tb_c964_6n); tb_c964rw += tb_c964_6n; memcpy(tb_c964rw, tb_c964_7, tb_c964_7n); tb_c964rw += tb_c964_7n; *tb_c964rw = 0; (const char *)tb_c964r; }));
    {
        tb_list * tb_l965 = ((tb_o_Node *)tb_nn(v_n, 951))->q_caps;
        for (long long tb_i965 = 0; tb_i965 < tb_l965->len; tb_i965++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l965->data)[tb_i965];
            if (((tb_o_Var *)tb_nn(v_v, 952))->q_is_shared) {
                { const char * tb_v966 = ((tb_o_Var *)tb_nn(v_v, 953))->q_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 953))->q_vec_names) = tb_v966; }
                { const char * tb_v967 = ({ const char *tb_c968_0 = "tb_a"; long long tb_c968_0n = sizeof("tb_a") - 1; const char *tb_c968_1 = v_id; long long tb_c968_1n = (long long)strlen(tb_c968_1); const char *tb_c968_2 = "_"; long long tb_c968_2n = sizeof("_") - 1; const char *tb_c968_3 = ((tb_o_Var *)tb_nn(v_v, 954))->q_name; long long tb_c968_3n = (long long)strlen(tb_c968_3); const char *tb_c968_4 = "_0"; long long tb_c968_4n = sizeof("_0") - 1; char *tb_c968r = tb_mem(tb_c968_0n + tb_c968_1n + tb_c968_2n + tb_c968_3n + tb_c968_4n + 1), *tb_c968rw = tb_c968r; memcpy(tb_c968rw, tb_c968_0, tb_c968_0n); tb_c968rw += tb_c968_0n; memcpy(tb_c968rw, tb_c968_1, tb_c968_1n); tb_c968rw += tb_c968_1n; memcpy(tb_c968rw, tb_c968_2, tb_c968_2n); tb_c968rw += tb_c968_2n; memcpy(tb_c968rw, tb_c968_3, tb_c968_3n); tb_c968rw += tb_c968_3n; memcpy(tb_c968rw, tb_c968_4, tb_c968_4n); tb_c968rw += tb_c968_4n; *tb_c968rw = 0; (const char *)tb_c968r; }); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 954))->q_vec_repl) = tb_v967; }
            }
        }
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 955))->q_kids, 1LL, sizeof(tb_o_Node *), 955)), (v_d + 1LL));
    tb_list * v_fresh_names2 = tb_list_new(sizeof(const char *));
    tb_list * v_fresh_repl2 = tb_list_new(sizeof(const char *));
    { tb_list * tb_v969 = v_fresh_names2; tb_list * *tb_p969 = &(((tb_o_Comp *)tb_nn(v_c, 958))->q_vec_names); *tb_p969 = tb_v969; }
    { tb_list * tb_v970 = v_fresh_repl2; tb_list * *tb_p970 = &(((tb_o_Comp *)tb_nn(v_c, 959))->q_vec_repl); *tb_p970 = tb_v970; }
    f_emit__indent(v_c, v_d);
    f_emit__put(v_c, "}\n");
    {
        tb_list * tb_l971 = ((tb_o_Node *)tb_nn(v_n, 963))->q_caps;
        for (long long tb_i971 = 0; tb_i971 < tb_l971->len; tb_i971++) {
            tb_o_Var * const v_v = ((tb_o_Var * *)tb_l971->data)[tb_i971];
            if (((tb_o_Var *)tb_nn(v_v, 964))->q_is_shared) {
                const char * v_target = ({ const char *tb_c972_0 = "v_"; long long tb_c972_0n = sizeof("v_") - 1; const char *tb_c972_1 = ((tb_o_Var *)tb_nn(v_v, 965))->q_name; long long tb_c972_1n = (long long)strlen(tb_c972_1); char *tb_c972r = tb_mem(tb_c972_0n + tb_c972_1n + 1), *tb_c972rw = tb_c972r; memcpy(tb_c972rw, tb_c972_0, tb_c972_0n); tb_c972rw += tb_c972_0n; memcpy(tb_c972rw, tb_c972_1, tb_c972_1n); tb_c972rw += tb_c972_1n; *tb_c972rw = 0; (const char *)tb_c972r; });
                if (((tb_o_Var *)tb_nn(v_v, 966))->q_is_link) {
                    v_target = ({ const char *tb_c973_0 = "(*v_"; long long tb_c973_0n = sizeof("(*v_") - 1; const char *tb_c973_1 = ((tb_o_Var *)tb_nn(v_v, 967))->q_name; long long tb_c973_1n = (long long)strlen(tb_c973_1); const char *tb_c973_2 = ")"; long long tb_c973_2n = sizeof(")") - 1; char *tb_c973r = tb_mem(tb_c973_0n + tb_c973_1n + tb_c973_2n + 1), *tb_c973rw = tb_c973r; memcpy(tb_c973rw, tb_c973_0, tb_c973_0n); tb_c973rw += tb_c973_0n; memcpy(tb_c973rw, tb_c973_1, tb_c973_1n); tb_c973rw += tb_c973_1n; memcpy(tb_c973rw, tb_c973_2, tb_c973_2n); tb_c973rw += tb_c973_2n; *tb_c973rw = 0; (const char *)tb_c973r; });
                }
                const char * v_a = ({ const char *tb_c974_0 = "tb_a"; long long tb_c974_0n = sizeof("tb_a") - 1; const char *tb_c974_1 = v_id; long long tb_c974_1n = (long long)strlen(tb_c974_1); const char *tb_c974_2 = "_"; long long tb_c974_2n = sizeof("_") - 1; const char *tb_c974_3 = ((tb_o_Var *)tb_nn(v_v, 968))->q_name; long long tb_c974_3n = (long long)strlen(tb_c974_3); const char *tb_c974_4 = "_"; long long tb_c974_4n = sizeof("_") - 1; char *tb_c974r = tb_mem(tb_c974_0n + tb_c974_1n + tb_c974_2n + tb_c974_3n + tb_c974_4n + 1), *tb_c974rw = tb_c974r; memcpy(tb_c974rw, tb_c974_0, tb_c974_0n); tb_c974rw += tb_c974_0n; memcpy(tb_c974rw, tb_c974_1, tb_c974_1n); tb_c974rw += tb_c974_1n; memcpy(tb_c974rw, tb_c974_2, tb_c974_2n); tb_c974rw += tb_c974_2n; memcpy(tb_c974rw, tb_c974_3, tb_c974_3n); tb_c974rw += tb_c974_3n; memcpy(tb_c974rw, tb_c974_4, tb_c974_4n); tb_c974rw += tb_c974_4n; *tb_c974rw = 0; (const char *)tb_c974r; });
                f_emit__indent(v_c, v_d);
                f_emit__put(v_c, ({ const char *tb_c975_0 = v_target; long long tb_c975_0n = (long long)strlen(tb_c975_0); const char *tb_c975_1 = " += (("; long long tb_c975_1n = sizeof(" += ((") - 1; const char *tb_c975_2 = v_a; long long tb_c975_2n = (long long)strlen(tb_c975_2); const char *tb_c975_3 = "0 + "; long long tb_c975_3n = sizeof("0 + ") - 1; const char *tb_c975_4 = v_a; long long tb_c975_4n = (long long)strlen(tb_c975_4); const char *tb_c975_5 = "1) + ("; long long tb_c975_5n = sizeof("1) + (") - 1; const char *tb_c975_6 = v_a; long long tb_c975_6n = (long long)strlen(tb_c975_6); const char *tb_c975_7 = "2 + "; long long tb_c975_7n = sizeof("2 + ") - 1; const char *tb_c975_8 = v_a; long long tb_c975_8n = (long long)strlen(tb_c975_8); const char *tb_c975_9 = "3)) + (("; long long tb_c975_9n = sizeof("3)) + ((") - 1; const char *tb_c975_10 = v_a; long long tb_c975_10n = (long long)strlen(tb_c975_10); const char *tb_c975_11 = "4 + "; long long tb_c975_11n = sizeof("4 + ") - 1; const char *tb_c975_12 = v_a; long long tb_c975_12n = (long long)strlen(tb_c975_12); const char *tb_c975_13 = "5) + ("; long long tb_c975_13n = sizeof("5) + (") - 1; const char *tb_c975_14 = v_a; long long tb_c975_14n = (long long)strlen(tb_c975_14); const char *tb_c975_15 = "6 + "; long long tb_c975_15n = sizeof("6 + ") - 1; const char *tb_c975_16 = v_a; long long tb_c975_16n = (long long)strlen(tb_c975_16); const char *tb_c975_17 = "7));\n"; long long tb_c975_17n = sizeof("7));\n") - 1; char *tb_c975r = tb_mem(tb_c975_0n + tb_c975_1n + tb_c975_2n + tb_c975_3n + tb_c975_4n + tb_c975_5n + tb_c975_6n + tb_c975_7n + tb_c975_8n + tb_c975_9n + tb_c975_10n + tb_c975_11n + tb_c975_12n + tb_c975_13n + tb_c975_14n + tb_c975_15n + tb_c975_16n + tb_c975_17n + 1), *tb_c975rw = tb_c975r; memcpy(tb_c975rw, tb_c975_0, tb_c975_0n); tb_c975rw += tb_c975_0n; memcpy(tb_c975rw, tb_c975_1, tb_c975_1n); tb_c975rw += tb_c975_1n; memcpy(tb_c975rw, tb_c975_2, tb_c975_2n); tb_c975rw += tb_c975_2n; memcpy(tb_c975rw, tb_c975_3, tb_c975_3n); tb_c975rw += tb_c975_3n; memcpy(tb_c975rw, tb_c975_4, tb_c975_4n); tb_c975rw += tb_c975_4n; memcpy(tb_c975rw, tb_c975_5, tb_c975_5n); tb_c975rw += tb_c975_5n; memcpy(tb_c975rw, tb_c975_6, tb_c975_6n); tb_c975rw += tb_c975_6n; memcpy(tb_c975rw, tb_c975_7, tb_c975_7n); tb_c975rw += tb_c975_7n; memcpy(tb_c975rw, tb_c975_8, tb_c975_8n); tb_c975rw += tb_c975_8n; memcpy(tb_c975rw, tb_c975_9, tb_c975_9n); tb_c975rw += tb_c975_9n; memcpy(tb_c975rw, tb_c975_10, tb_c975_10n); tb_c975rw += tb_c975_10n; memcpy(tb_c975rw, tb_c975_11, tb_c975_11n); tb_c975rw += tb_c975_11n; memcpy(tb_c975rw, tb_c975_12, tb_c975_12n); tb_c975rw += tb_c975_12n; memcpy(tb_c975rw, tb_c975_13, tb_c975_13n); tb_c975rw += tb_c975_13n; memcpy(tb_c975rw, tb_c975_14, tb_c975_14n); tb_c975rw += tb_c975_14n; memcpy(tb_c975rw, tb_c975_15, tb_c975_15n); tb_c975rw += tb_c975_15n; memcpy(tb_c975rw, tb_c975_16, tb_c975_16n); tb_c975rw += tb_c975_16n; memcpy(tb_c975rw, tb_c975_17, tb_c975_17n); tb_c975rw += tb_c975_17n; *tb_c975rw = 0; (const char *)tb_c975r; }));
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
    { long long tb_v976 = 1LL; long long *tb_p976 = &(((tb_o_Comp *)tb_nn(v_c, 984))->q_label); *tb_p976 += tb_v976; }
    const char * v_id = ({ const char *tb_c977_0 = ""; long long tb_c977_0n = sizeof("") - 1; char tb_c977_1d[24]; long long tb_c977_1n; const char *tb_c977_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 985))->q_label, tb_c977_1d, &tb_c977_1n); char *tb_c977r = tb_mem(tb_c977_0n + tb_c977_1n + 1), *tb_c977rw = tb_c977r; memcpy(tb_c977rw, tb_c977_0, tb_c977_0n); tb_c977rw += tb_c977_0n; memcpy(tb_c977rw, tb_c977_1, tb_c977_1n); tb_c977rw += tb_c977_1n; *tb_c977rw = 0; (const char *)tb_c977r; });
    long long v_t = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 986))->q_kids, 0LL, sizeof(tb_o_Node *), 986)), 986))->q_ty;
    const char * v_k = f_check__kind_of(v_c, v_t);
    long long v_elem_ty = (-1LL);
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    if (({ const char *tb_s978 = v_k; ((unsigned char)tb_s978[0] == 100 && (unsigned char)tb_s978[1] == 105 && (unsigned char)tb_s978[2] == 99 && (unsigned char)tb_s978[3] == 116 && tb_s978[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c979_0 = "tb_list * tb_l"; long long tb_c979_0n = sizeof("tb_list * tb_l") - 1; const char *tb_c979_1 = v_id; long long tb_c979_1n = (long long)strlen(tb_c979_1); const char *tb_c979_2 = " = ("; long long tb_c979_2n = sizeof(" = (") - 1; char *tb_c979r = tb_mem(tb_c979_0n + tb_c979_1n + tb_c979_2n + 1), *tb_c979rw = tb_c979r; memcpy(tb_c979rw, tb_c979_0, tb_c979_0n); tb_c979rw += tb_c979_0n; memcpy(tb_c979rw, tb_c979_1, tb_c979_1n); tb_c979rw += tb_c979_1n; memcpy(tb_c979rw, tb_c979_2, tb_c979_2n); tb_c979rw += tb_c979_2n; *tb_c979rw = 0; (const char *)tb_c979r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 994))->q_kids, 0LL, sizeof(tb_o_Node *), 994)));
        f_emit__put(v_c, ")->keys;\n");
        v_k = "list";
        v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 997))->q_types, v_t, sizeof(tb_o_Type *), 997)), 997))->q_key;
    } else {
        f_emit__put(v_c, ({ const char *tb_c980_0 = f_emit__c_type(v_c, v_t); long long tb_c980_0n = (long long)strlen(tb_c980_0); const char *tb_c980_1 = " tb_l"; long long tb_c980_1n = sizeof(" tb_l") - 1; const char *tb_c980_2 = v_id; long long tb_c980_2n = (long long)strlen(tb_c980_2); const char *tb_c980_3 = " = "; long long tb_c980_3n = sizeof(" = ") - 1; char *tb_c980r = tb_mem(tb_c980_0n + tb_c980_1n + tb_c980_2n + tb_c980_3n + 1), *tb_c980rw = tb_c980r; memcpy(tb_c980rw, tb_c980_0, tb_c980_0n); tb_c980rw += tb_c980_0n; memcpy(tb_c980rw, tb_c980_1, tb_c980_1n); tb_c980rw += tb_c980_1n; memcpy(tb_c980rw, tb_c980_2, tb_c980_2n); tb_c980rw += tb_c980_2n; memcpy(tb_c980rw, tb_c980_3, tb_c980_3n); tb_c980rw += tb_c980_3n; *tb_c980rw = 0; (const char *)tb_c980r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1000))->q_kids, 0LL, sizeof(tb_o_Node *), 1000)));
        f_emit__put(v_c, ";\n");
        if (({ const char *tb_s981 = v_k; ((unsigned char)tb_s981[0] == 108 && (unsigned char)tb_s981[1] == 105 && (unsigned char)tb_s981[2] == 115 && (unsigned char)tb_s981[3] == 116 && tb_s981[4] == 0); })) {
            v_elem_ty = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1003))->q_types, v_t, sizeof(tb_o_Type *), 1003)), 1003))->q_elem;
        }
    }
    f_emit__indent(v_c, (v_depth + 1LL));
    if (({ const char *tb_s982 = v_k; ((unsigned char)tb_s982[0] == 116 && (unsigned char)tb_s982[1] == 101 && (unsigned char)tb_s982[2] == 120 && (unsigned char)tb_s982[3] == 116 && tb_s982[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c983_0 = "for (long long tb_i"; long long tb_c983_0n = sizeof("for (long long tb_i") - 1; const char *tb_c983_1 = v_id; long long tb_c983_1n = (long long)strlen(tb_c983_1); const char *tb_c983_2 = " = 0; tb_l"; long long tb_c983_2n = sizeof(" = 0; tb_l") - 1; const char *tb_c983_3 = v_id; long long tb_c983_3n = (long long)strlen(tb_c983_3); const char *tb_c983_4 = "[tb_i"; long long tb_c983_4n = sizeof("[tb_i") - 1; const char *tb_c983_5 = v_id; long long tb_c983_5n = (long long)strlen(tb_c983_5); const char *tb_c983_6 = "] != 0; tb_i"; long long tb_c983_6n = sizeof("] != 0; tb_i") - 1; const char *tb_c983_7 = v_id; long long tb_c983_7n = (long long)strlen(tb_c983_7); const char *tb_c983_8 = "++) {\n"; long long tb_c983_8n = sizeof("++) {\n") - 1; char *tb_c983r = tb_mem(tb_c983_0n + tb_c983_1n + tb_c983_2n + tb_c983_3n + tb_c983_4n + tb_c983_5n + tb_c983_6n + tb_c983_7n + tb_c983_8n + 1), *tb_c983rw = tb_c983r; memcpy(tb_c983rw, tb_c983_0, tb_c983_0n); tb_c983rw += tb_c983_0n; memcpy(tb_c983rw, tb_c983_1, tb_c983_1n); tb_c983rw += tb_c983_1n; memcpy(tb_c983rw, tb_c983_2, tb_c983_2n); tb_c983rw += tb_c983_2n; memcpy(tb_c983rw, tb_c983_3, tb_c983_3n); tb_c983rw += tb_c983_3n; memcpy(tb_c983rw, tb_c983_4, tb_c983_4n); tb_c983rw += tb_c983_4n; memcpy(tb_c983rw, tb_c983_5, tb_c983_5n); tb_c983rw += tb_c983_5n; memcpy(tb_c983rw, tb_c983_6, tb_c983_6n); tb_c983rw += tb_c983_6n; memcpy(tb_c983rw, tb_c983_7, tb_c983_7n); tb_c983rw += tb_c983_7n; memcpy(tb_c983rw, tb_c983_8, tb_c983_8n); tb_c983rw += tb_c983_8n; *tb_c983rw = 0; (const char *)tb_c983r; }));
        f_emit__indent(v_c, (v_depth + 2LL));
        f_emit__put(v_c, ({ const char *tb_c984_0 = "const char * const v_"; long long tb_c984_0n = sizeof("const char * const v_") - 1; const char *tb_c984_1 = ((tb_o_Node *)tb_nn(v_n, 1008))->q_name; long long tb_c984_1n = (long long)strlen(tb_c984_1); const char *tb_c984_2 = " = tb_chr((unsigned char)tb_l"; long long tb_c984_2n = sizeof(" = tb_chr((unsigned char)tb_l") - 1; const char *tb_c984_3 = v_id; long long tb_c984_3n = (long long)strlen(tb_c984_3); const char *tb_c984_4 = "[tb_i"; long long tb_c984_4n = sizeof("[tb_i") - 1; const char *tb_c984_5 = v_id; long long tb_c984_5n = (long long)strlen(tb_c984_5); const char *tb_c984_6 = "]);\n"; long long tb_c984_6n = sizeof("]);\n") - 1; char *tb_c984r = tb_mem(tb_c984_0n + tb_c984_1n + tb_c984_2n + tb_c984_3n + tb_c984_4n + tb_c984_5n + tb_c984_6n + 1), *tb_c984rw = tb_c984r; memcpy(tb_c984rw, tb_c984_0, tb_c984_0n); tb_c984rw += tb_c984_0n; memcpy(tb_c984rw, tb_c984_1, tb_c984_1n); tb_c984rw += tb_c984_1n; memcpy(tb_c984rw, tb_c984_2, tb_c984_2n); tb_c984rw += tb_c984_2n; memcpy(tb_c984rw, tb_c984_3, tb_c984_3n); tb_c984rw += tb_c984_3n; memcpy(tb_c984rw, tb_c984_4, tb_c984_4n); tb_c984rw += tb_c984_4n; memcpy(tb_c984rw, tb_c984_5, tb_c984_5n); tb_c984rw += tb_c984_5n; memcpy(tb_c984rw, tb_c984_6, tb_c984_6n); tb_c984rw += tb_c984_6n; *tb_c984rw = 0; (const char *)tb_c984r; }));
    } else {
        const char * v_elem = f_emit__c_type(v_c, v_elem_ty);
        f_emit__put(v_c, ({ const char *tb_c985_0 = "for (long long tb_i"; long long tb_c985_0n = sizeof("for (long long tb_i") - 1; const char *tb_c985_1 = v_id; long long tb_c985_1n = (long long)strlen(tb_c985_1); const char *tb_c985_2 = " = 0; tb_i"; long long tb_c985_2n = sizeof(" = 0; tb_i") - 1; const char *tb_c985_3 = v_id; long long tb_c985_3n = (long long)strlen(tb_c985_3); const char *tb_c985_4 = " < tb_l"; long long tb_c985_4n = sizeof(" < tb_l") - 1; const char *tb_c985_5 = v_id; long long tb_c985_5n = (long long)strlen(tb_c985_5); const char *tb_c985_6 = "->len; tb_i"; long long tb_c985_6n = sizeof("->len; tb_i") - 1; const char *tb_c985_7 = v_id; long long tb_c985_7n = (long long)strlen(tb_c985_7); const char *tb_c985_8 = "++) {\n"; long long tb_c985_8n = sizeof("++) {\n") - 1; char *tb_c985r = tb_mem(tb_c985_0n + tb_c985_1n + tb_c985_2n + tb_c985_3n + tb_c985_4n + tb_c985_5n + tb_c985_6n + tb_c985_7n + tb_c985_8n + 1), *tb_c985rw = tb_c985r; memcpy(tb_c985rw, tb_c985_0, tb_c985_0n); tb_c985rw += tb_c985_0n; memcpy(tb_c985rw, tb_c985_1, tb_c985_1n); tb_c985rw += tb_c985_1n; memcpy(tb_c985rw, tb_c985_2, tb_c985_2n); tb_c985rw += tb_c985_2n; memcpy(tb_c985rw, tb_c985_3, tb_c985_3n); tb_c985rw += tb_c985_3n; memcpy(tb_c985rw, tb_c985_4, tb_c985_4n); tb_c985rw += tb_c985_4n; memcpy(tb_c985rw, tb_c985_5, tb_c985_5n); tb_c985rw += tb_c985_5n; memcpy(tb_c985rw, tb_c985_6, tb_c985_6n); tb_c985rw += tb_c985_6n; memcpy(tb_c985rw, tb_c985_7, tb_c985_7n); tb_c985rw += tb_c985_7n; memcpy(tb_c985rw, tb_c985_8, tb_c985_8n); tb_c985rw += tb_c985_8n; *tb_c985rw = 0; (const char *)tb_c985r; }));
        f_emit__indent(v_c, (v_depth + 2LL));
        if (f_check__is_cols_obj(v_c, v_elem_ty)) {
            f_emit__put(v_c, ({ const char *tb_c986_0 = v_elem; long long tb_c986_0n = (long long)strlen(tb_c986_0); const char *tb_c986_1 = " const v_"; long long tb_c986_1n = sizeof(" const v_") - 1; const char *tb_c986_2 = ((tb_o_Node *)tb_nn(v_n, 1014))->q_name; long long tb_c986_2n = (long long)strlen(tb_c986_2); const char *tb_c986_3 = " = {tb_l"; long long tb_c986_3n = sizeof(" = {tb_l") - 1; const char *tb_c986_4 = v_id; long long tb_c986_4n = (long long)strlen(tb_c986_4); const char *tb_c986_5 = ", tb_i"; long long tb_c986_5n = sizeof(", tb_i") - 1; const char *tb_c986_6 = v_id; long long tb_c986_6n = (long long)strlen(tb_c986_6); const char *tb_c986_7 = "};\n"; long long tb_c986_7n = sizeof("};\n") - 1; char *tb_c986r = tb_mem(tb_c986_0n + tb_c986_1n + tb_c986_2n + tb_c986_3n + tb_c986_4n + tb_c986_5n + tb_c986_6n + tb_c986_7n + 1), *tb_c986rw = tb_c986r; memcpy(tb_c986rw, tb_c986_0, tb_c986_0n); tb_c986rw += tb_c986_0n; memcpy(tb_c986rw, tb_c986_1, tb_c986_1n); tb_c986rw += tb_c986_1n; memcpy(tb_c986rw, tb_c986_2, tb_c986_2n); tb_c986rw += tb_c986_2n; memcpy(tb_c986rw, tb_c986_3, tb_c986_3n); tb_c986rw += tb_c986_3n; memcpy(tb_c986rw, tb_c986_4, tb_c986_4n); tb_c986rw += tb_c986_4n; memcpy(tb_c986rw, tb_c986_5, tb_c986_5n); tb_c986rw += tb_c986_5n; memcpy(tb_c986rw, tb_c986_6, tb_c986_6n); tb_c986rw += tb_c986_6n; memcpy(tb_c986rw, tb_c986_7, tb_c986_7n); tb_c986rw += tb_c986_7n; *tb_c986rw = 0; (const char *)tb_c986r; }));
        } else if ((v_elem_ty == 3LL)) {
            f_emit__put(v_c, ({ const char *tb_c987_0 = "_Bool const v_"; long long tb_c987_0n = sizeof("_Bool const v_") - 1; const char *tb_c987_1 = ((tb_o_Node *)tb_nn(v_n, 1016))->q_name; long long tb_c987_1n = (long long)strlen(tb_c987_1); const char *tb_c987_2 = " = TB_BIT(tb_l"; long long tb_c987_2n = sizeof(" = TB_BIT(tb_l") - 1; const char *tb_c987_3 = v_id; long long tb_c987_3n = (long long)strlen(tb_c987_3); const char *tb_c987_4 = ", tb_i"; long long tb_c987_4n = sizeof(", tb_i") - 1; const char *tb_c987_5 = v_id; long long tb_c987_5n = (long long)strlen(tb_c987_5); const char *tb_c987_6 = ");\n"; long long tb_c987_6n = sizeof(");\n") - 1; char *tb_c987r = tb_mem(tb_c987_0n + tb_c987_1n + tb_c987_2n + tb_c987_3n + tb_c987_4n + tb_c987_5n + tb_c987_6n + 1), *tb_c987rw = tb_c987r; memcpy(tb_c987rw, tb_c987_0, tb_c987_0n); tb_c987rw += tb_c987_0n; memcpy(tb_c987rw, tb_c987_1, tb_c987_1n); tb_c987rw += tb_c987_1n; memcpy(tb_c987rw, tb_c987_2, tb_c987_2n); tb_c987rw += tb_c987_2n; memcpy(tb_c987rw, tb_c987_3, tb_c987_3n); tb_c987rw += tb_c987_3n; memcpy(tb_c987rw, tb_c987_4, tb_c987_4n); tb_c987rw += tb_c987_4n; memcpy(tb_c987rw, tb_c987_5, tb_c987_5n); tb_c987rw += tb_c987_5n; memcpy(tb_c987rw, tb_c987_6, tb_c987_6n); tb_c987rw += tb_c987_6n; *tb_c987rw = 0; (const char *)tb_c987r; }));
        } else {
            f_emit__put(v_c, ({ const char *tb_c988_0 = v_elem; long long tb_c988_0n = (long long)strlen(tb_c988_0); const char *tb_c988_1 = " const v_"; long long tb_c988_1n = sizeof(" const v_") - 1; const char *tb_c988_2 = ((tb_o_Node *)tb_nn(v_n, 1018))->q_name; long long tb_c988_2n = (long long)strlen(tb_c988_2); const char *tb_c988_3 = " = (("; long long tb_c988_3n = sizeof(" = ((") - 1; const char *tb_c988_4 = v_elem; long long tb_c988_4n = (long long)strlen(tb_c988_4); const char *tb_c988_5 = " *)tb_l"; long long tb_c988_5n = sizeof(" *)tb_l") - 1; const char *tb_c988_6 = v_id; long long tb_c988_6n = (long long)strlen(tb_c988_6); const char *tb_c988_7 = "->data)[tb_i"; long long tb_c988_7n = sizeof("->data)[tb_i") - 1; const char *tb_c988_8 = v_id; long long tb_c988_8n = (long long)strlen(tb_c988_8); const char *tb_c988_9 = "];\n"; long long tb_c988_9n = sizeof("];\n") - 1; char *tb_c988r = tb_mem(tb_c988_0n + tb_c988_1n + tb_c988_2n + tb_c988_3n + tb_c988_4n + tb_c988_5n + tb_c988_6n + tb_c988_7n + tb_c988_8n + tb_c988_9n + 1), *tb_c988rw = tb_c988r; memcpy(tb_c988rw, tb_c988_0, tb_c988_0n); tb_c988rw += tb_c988_0n; memcpy(tb_c988rw, tb_c988_1, tb_c988_1n); tb_c988rw += tb_c988_1n; memcpy(tb_c988rw, tb_c988_2, tb_c988_2n); tb_c988rw += tb_c988_2n; memcpy(tb_c988rw, tb_c988_3, tb_c988_3n); tb_c988rw += tb_c988_3n; memcpy(tb_c988rw, tb_c988_4, tb_c988_4n); tb_c988rw += tb_c988_4n; memcpy(tb_c988rw, tb_c988_5, tb_c988_5n); tb_c988rw += tb_c988_5n; memcpy(tb_c988rw, tb_c988_6, tb_c988_6n); tb_c988rw += tb_c988_6n; memcpy(tb_c988rw, tb_c988_7, tb_c988_7n); tb_c988rw += tb_c988_7n; memcpy(tb_c988rw, tb_c988_8, tb_c988_8n); tb_c988rw += tb_c988_8n; memcpy(tb_c988rw, tb_c988_9, tb_c988_9n); tb_c988rw += tb_c988_9n; *tb_c988rw = 0; (const char *)tb_c988r; }));
            f_emit__emit_prefetch(v_c, v_n, (v_depth + 2LL), ({ const char *tb_c989_0 = "tb_l"; long long tb_c989_0n = sizeof("tb_l") - 1; const char *tb_c989_1 = v_id; long long tb_c989_1n = (long long)strlen(tb_c989_1); char *tb_c989r = tb_mem(tb_c989_0n + tb_c989_1n + 1), *tb_c989rw = tb_c989r; memcpy(tb_c989rw, tb_c989_0, tb_c989_0n); tb_c989rw += tb_c989_0n; memcpy(tb_c989rw, tb_c989_1, tb_c989_1n); tb_c989rw += tb_c989_1n; *tb_c989rw = 0; (const char *)tb_c989r; }), ({ const char *tb_c990_0 = "tb_i"; long long tb_c990_0n = sizeof("tb_i") - 1; const char *tb_c990_1 = v_id; long long tb_c990_1n = (long long)strlen(tb_c990_1); char *tb_c990r = tb_mem(tb_c990_0n + tb_c990_1n + 1), *tb_c990rw = tb_c990r; memcpy(tb_c990rw, tb_c990_0, tb_c990_0n); tb_c990rw += tb_c990_0n; memcpy(tb_c990rw, tb_c990_1, tb_c990_1n); tb_c990rw += tb_c990_1n; *tb_c990rw = 0; (const char *)tb_c990r; }), v_elem);
        }
    }
    f_emit__emit_loop_body(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1020))->q_kids, 1LL, sizeof(tb_o_Node *), 1020)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_pieces(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v991 = 1LL; long long *tb_p991 = &(((tb_o_Comp *)tb_nn(v_c, 1031))->q_label); *tb_p991 += tb_v991; }
    const char * v_id = ({ const char *tb_c992_0 = ""; long long tb_c992_0n = sizeof("") - 1; char tb_c992_1d[24]; long long tb_c992_1n; const char *tb_c992_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1032))->q_label, tb_c992_1d, &tb_c992_1n); char *tb_c992r = tb_mem(tb_c992_0n + tb_c992_1n + 1), *tb_c992rw = tb_c992r; memcpy(tb_c992rw, tb_c992_0, tb_c992_0n); tb_c992rw += tb_c992_0n; memcpy(tb_c992rw, tb_c992_1, tb_c992_1n); tb_c992rw += tb_c992_1n; *tb_c992rw = 0; (const char *)tb_c992r; });
    const char * v_l = ({ const char *tb_c993_0 = "tb_l"; long long tb_c993_0n = sizeof("tb_l") - 1; const char *tb_c993_1 = v_id; long long tb_c993_1n = (long long)strlen(tb_c993_1); char *tb_c993r = tb_mem(tb_c993_0n + tb_c993_1n + 1), *tb_c993rw = tb_c993r; memcpy(tb_c993rw, tb_c993_0, tb_c993_0n); tb_c993rw += tb_c993_0n; memcpy(tb_c993rw, tb_c993_1, tb_c993_1n); tb_c993rw += tb_c993_1n; *tb_c993rw = 0; (const char *)tb_c993r; });
    const char * v_i = ({ const char *tb_c994_0 = "tb_i"; long long tb_c994_0n = sizeof("tb_i") - 1; const char *tb_c994_1 = v_id; long long tb_c994_1n = (long long)strlen(tb_c994_1); char *tb_c994r = tb_mem(tb_c994_0n + tb_c994_1n + 1), *tb_c994rw = tb_c994r; memcpy(tb_c994rw, tb_c994_0, tb_c994_0n); tb_c994rw += tb_c994_0n; memcpy(tb_c994rw, tb_c994_1, tb_c994_1n); tb_c994rw += tb_c994_1n; *tb_c994rw = 0; (const char *)tb_c994r; });
    const char * v_p = ({ const char *tb_c995_0 = "tb_p"; long long tb_c995_0n = sizeof("tb_p") - 1; const char *tb_c995_1 = v_id; long long tb_c995_1n = (long long)strlen(tb_c995_1); char *tb_c995r = tb_mem(tb_c995_0n + tb_c995_1n + 1), *tb_c995rw = tb_c995r; memcpy(tb_c995rw, tb_c995_0, tb_c995_0n); tb_c995rw += tb_c995_0n; memcpy(tb_c995rw, tb_c995_1, tb_c995_1n); tb_c995rw += tb_c995_1n; *tb_c995rw = 0; (const char *)tb_c995r; });
    const char * v_m = ({ const char *tb_c996_0 = "tb_n"; long long tb_c996_0n = sizeof("tb_n") - 1; const char *tb_c996_1 = v_id; long long tb_c996_1n = (long long)strlen(tb_c996_1); char *tb_c996r = tb_mem(tb_c996_0n + tb_c996_1n + 1), *tb_c996rw = tb_c996r; memcpy(tb_c996rw, tb_c996_0, tb_c996_0n); tb_c996rw += tb_c996_0n; memcpy(tb_c996rw, tb_c996_1, tb_c996_1n); tb_c996rw += tb_c996_1n; *tb_c996rw = 0; (const char *)tb_c996r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c997_0 = "tb_list *"; long long tb_c997_0n = sizeof("tb_list *") - 1; const char *tb_c997_1 = v_l; long long tb_c997_1n = (long long)strlen(tb_c997_1); const char *tb_c997_2 = " = "; long long tb_c997_2n = sizeof(" = ") - 1; char *tb_c997r = tb_mem(tb_c997_0n + tb_c997_1n + tb_c997_2n + 1), *tb_c997rw = tb_c997r; memcpy(tb_c997rw, tb_c997_0, tb_c997_0n); tb_c997rw += tb_c997_0n; memcpy(tb_c997rw, tb_c997_1, tb_c997_1n); tb_c997rw += tb_c997_1n; memcpy(tb_c997rw, tb_c997_2, tb_c997_2n); tb_c997rw += tb_c997_2n; *tb_c997rw = 0; (const char *)tb_c997r; }));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1041))->q_kids, 0LL, sizeof(tb_o_Node *), 1041)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c998_0 = "long long "; long long tb_c998_0n = sizeof("long long ") - 1; const char *tb_c998_1 = v_m; long long tb_c998_1n = (long long)strlen(tb_c998_1); const char *tb_c998_2 = " = "; long long tb_c998_2n = sizeof(" = ") - 1; char *tb_c998r = tb_mem(tb_c998_0n + tb_c998_1n + tb_c998_2n + 1), *tb_c998rw = tb_c998r; memcpy(tb_c998rw, tb_c998_0, tb_c998_0n); tb_c998rw += tb_c998_0n; memcpy(tb_c998rw, tb_c998_1, tb_c998_1n); tb_c998rw += tb_c998_1n; memcpy(tb_c998rw, tb_c998_2, tb_c998_2n); tb_c998rw += tb_c998_2n; *tb_c998rw = 0; (const char *)tb_c998r; }));
    f_emit__emit_expr(v_c, ((tb_o_Node *)tb_nn(v_n, 1045))->q_piece_size);
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c999_0 = "if ("; long long tb_c999_0n = sizeof("if (") - 1; const char *tb_c999_1 = v_m; long long tb_c999_1n = (long long)strlen(tb_c999_1); const char *tb_c999_2 = " <= 0) tb_crash("; long long tb_c999_2n = sizeof(" <= 0) tb_crash(") - 1; char tb_c999_3d[24]; long long tb_c999_3n; const char *tb_c999_3 = tb_digits(((tb_o_Node *)tb_nn(v_n, 1048))->q_line, tb_c999_3d, &tb_c999_3n); const char *tb_c999_4 = ", \"размер куска в by должен быть больше 0\");\n"; long long tb_c999_4n = sizeof(", \"размер куска в by должен быть больше 0\");\n") - 1; char *tb_c999r = tb_mem(tb_c999_0n + tb_c999_1n + tb_c999_2n + tb_c999_3n + tb_c999_4n + 1), *tb_c999rw = tb_c999r; memcpy(tb_c999rw, tb_c999_0, tb_c999_0n); tb_c999rw += tb_c999_0n; memcpy(tb_c999rw, tb_c999_1, tb_c999_1n); tb_c999rw += tb_c999_1n; memcpy(tb_c999rw, tb_c999_2, tb_c999_2n); tb_c999rw += tb_c999_2n; memcpy(tb_c999rw, tb_c999_3, tb_c999_3n); tb_c999rw += tb_c999_3n; memcpy(tb_c999rw, tb_c999_4, tb_c999_4n); tb_c999rw += tb_c999_4n; *tb_c999rw = 0; (const char *)tb_c999r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c1000_0 = "tb_list "; long long tb_c1000_0n = sizeof("tb_list ") - 1; const char *tb_c1000_1 = v_p; long long tb_c1000_1n = (long long)strlen(tb_c1000_1); const char *tb_c1000_2 = " = {0, 0, -1, "; long long tb_c1000_2n = sizeof(" = {0, 0, -1, ") - 1; const char *tb_c1000_3 = v_l; long long tb_c1000_3n = (long long)strlen(tb_c1000_3); const char *tb_c1000_4 = "->size};\n"; long long tb_c1000_4n = sizeof("->size};\n") - 1; char *tb_c1000r = tb_mem(tb_c1000_0n + tb_c1000_1n + tb_c1000_2n + tb_c1000_3n + tb_c1000_4n + 1), *tb_c1000rw = tb_c1000r; memcpy(tb_c1000rw, tb_c1000_0, tb_c1000_0n); tb_c1000rw += tb_c1000_0n; memcpy(tb_c1000rw, tb_c1000_1, tb_c1000_1n); tb_c1000rw += tb_c1000_1n; memcpy(tb_c1000rw, tb_c1000_2, tb_c1000_2n); tb_c1000rw += tb_c1000_2n; memcpy(tb_c1000rw, tb_c1000_3, tb_c1000_3n); tb_c1000rw += tb_c1000_3n; memcpy(tb_c1000rw, tb_c1000_4, tb_c1000_4n); tb_c1000rw += tb_c1000_4n; *tb_c1000rw = 0; (const char *)tb_c1000r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c1001_0 = "long long tb_cap"; long long tb_c1001_0n = sizeof("long long tb_cap") - 1; const char *tb_c1001_1 = v_id; long long tb_c1001_1n = (long long)strlen(tb_c1001_1); const char *tb_c1001_2 = " = "; long long tb_c1001_2n = sizeof(" = ") - 1; const char *tb_c1001_3 = v_l; long long tb_c1001_3n = (long long)strlen(tb_c1001_3); const char *tb_c1001_4 = "->cap;\n"; long long tb_c1001_4n = sizeof("->cap;\n") - 1; char *tb_c1001r = tb_mem(tb_c1001_0n + tb_c1001_1n + tb_c1001_2n + tb_c1001_3n + tb_c1001_4n + 1), *tb_c1001rw = tb_c1001r; memcpy(tb_c1001rw, tb_c1001_0, tb_c1001_0n); tb_c1001rw += tb_c1001_0n; memcpy(tb_c1001rw, tb_c1001_1, tb_c1001_1n); tb_c1001rw += tb_c1001_1n; memcpy(tb_c1001rw, tb_c1001_2, tb_c1001_2n); tb_c1001rw += tb_c1001_2n; memcpy(tb_c1001rw, tb_c1001_3, tb_c1001_3n); tb_c1001rw += tb_c1001_3n; memcpy(tb_c1001rw, tb_c1001_4, tb_c1001_4n); tb_c1001rw += tb_c1001_4n; *tb_c1001rw = 0; (const char *)tb_c1001r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c1002_0 = "if ("; long long tb_c1002_0n = sizeof("if (") - 1; const char *tb_c1002_1 = v_l; long long tb_c1002_1n = (long long)strlen(tb_c1002_1); const char *tb_c1002_2 = "->cap >= 0) "; long long tb_c1002_2n = sizeof("->cap >= 0) ") - 1; const char *tb_c1002_3 = v_l; long long tb_c1002_3n = (long long)strlen(tb_c1002_3); const char *tb_c1002_4 = "->cap = -2;\n"; long long tb_c1002_4n = sizeof("->cap = -2;\n") - 1; char *tb_c1002r = tb_mem(tb_c1002_0n + tb_c1002_1n + tb_c1002_2n + tb_c1002_3n + tb_c1002_4n + 1), *tb_c1002rw = tb_c1002r; memcpy(tb_c1002rw, tb_c1002_0, tb_c1002_0n); tb_c1002rw += tb_c1002_0n; memcpy(tb_c1002rw, tb_c1002_1, tb_c1002_1n); tb_c1002rw += tb_c1002_1n; memcpy(tb_c1002rw, tb_c1002_2, tb_c1002_2n); tb_c1002rw += tb_c1002_2n; memcpy(tb_c1002rw, tb_c1002_3, tb_c1002_3n); tb_c1002rw += tb_c1002_3n; memcpy(tb_c1002rw, tb_c1002_4, tb_c1002_4n); tb_c1002rw += tb_c1002_4n; *tb_c1002rw = 0; (const char *)tb_c1002r; }));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c1003_0 = "for (long long "; long long tb_c1003_0n = sizeof("for (long long ") - 1; const char *tb_c1003_1 = v_i; long long tb_c1003_1n = (long long)strlen(tb_c1003_1); const char *tb_c1003_2 = " = 0; "; long long tb_c1003_2n = sizeof(" = 0; ") - 1; const char *tb_c1003_3 = v_i; long long tb_c1003_3n = (long long)strlen(tb_c1003_3); const char *tb_c1003_4 = " < "; long long tb_c1003_4n = sizeof(" < ") - 1; const char *tb_c1003_5 = v_l; long long tb_c1003_5n = (long long)strlen(tb_c1003_5); const char *tb_c1003_6 = "->len; "; long long tb_c1003_6n = sizeof("->len; ") - 1; const char *tb_c1003_7 = v_i; long long tb_c1003_7n = (long long)strlen(tb_c1003_7); const char *tb_c1003_8 = " += "; long long tb_c1003_8n = sizeof(" += ") - 1; const char *tb_c1003_9 = v_m; long long tb_c1003_9n = (long long)strlen(tb_c1003_9); const char *tb_c1003_10 = ") {\n"; long long tb_c1003_10n = sizeof(") {\n") - 1; char *tb_c1003r = tb_mem(tb_c1003_0n + tb_c1003_1n + tb_c1003_2n + tb_c1003_3n + tb_c1003_4n + tb_c1003_5n + tb_c1003_6n + tb_c1003_7n + tb_c1003_8n + tb_c1003_9n + tb_c1003_10n + 1), *tb_c1003rw = tb_c1003r; memcpy(tb_c1003rw, tb_c1003_0, tb_c1003_0n); tb_c1003rw += tb_c1003_0n; memcpy(tb_c1003rw, tb_c1003_1, tb_c1003_1n); tb_c1003rw += tb_c1003_1n; memcpy(tb_c1003rw, tb_c1003_2, tb_c1003_2n); tb_c1003rw += tb_c1003_2n; memcpy(tb_c1003rw, tb_c1003_3, tb_c1003_3n); tb_c1003rw += tb_c1003_3n; memcpy(tb_c1003rw, tb_c1003_4, tb_c1003_4n); tb_c1003rw += tb_c1003_4n; memcpy(tb_c1003rw, tb_c1003_5, tb_c1003_5n); tb_c1003rw += tb_c1003_5n; memcpy(tb_c1003rw, tb_c1003_6, tb_c1003_6n); tb_c1003rw += tb_c1003_6n; memcpy(tb_c1003rw, tb_c1003_7, tb_c1003_7n); tb_c1003rw += tb_c1003_7n; memcpy(tb_c1003rw, tb_c1003_8, tb_c1003_8n); tb_c1003rw += tb_c1003_8n; memcpy(tb_c1003rw, tb_c1003_9, tb_c1003_9n); tb_c1003rw += tb_c1003_9n; memcpy(tb_c1003rw, tb_c1003_10, tb_c1003_10n); tb_c1003rw += tb_c1003_10n; *tb_c1003rw = 0; (const char *)tb_c1003r; }));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, ({ const char *tb_c1004_0 = v_p; long long tb_c1004_0n = (long long)strlen(tb_c1004_0); const char *tb_c1004_1 = ".data = "; long long tb_c1004_1n = sizeof(".data = ") - 1; const char *tb_c1004_2 = v_l; long long tb_c1004_2n = (long long)strlen(tb_c1004_2); const char *tb_c1004_3 = "->data + "; long long tb_c1004_3n = sizeof("->data + ") - 1; const char *tb_c1004_4 = v_i; long long tb_c1004_4n = (long long)strlen(tb_c1004_4); const char *tb_c1004_5 = " * "; long long tb_c1004_5n = sizeof(" * ") - 1; const char *tb_c1004_6 = v_l; long long tb_c1004_6n = (long long)strlen(tb_c1004_6); const char *tb_c1004_7 = "->size;\n"; long long tb_c1004_7n = sizeof("->size;\n") - 1; char *tb_c1004r = tb_mem(tb_c1004_0n + tb_c1004_1n + tb_c1004_2n + tb_c1004_3n + tb_c1004_4n + tb_c1004_5n + tb_c1004_6n + tb_c1004_7n + 1), *tb_c1004rw = tb_c1004r; memcpy(tb_c1004rw, tb_c1004_0, tb_c1004_0n); tb_c1004rw += tb_c1004_0n; memcpy(tb_c1004rw, tb_c1004_1, tb_c1004_1n); tb_c1004rw += tb_c1004_1n; memcpy(tb_c1004rw, tb_c1004_2, tb_c1004_2n); tb_c1004rw += tb_c1004_2n; memcpy(tb_c1004rw, tb_c1004_3, tb_c1004_3n); tb_c1004rw += tb_c1004_3n; memcpy(tb_c1004rw, tb_c1004_4, tb_c1004_4n); tb_c1004rw += tb_c1004_4n; memcpy(tb_c1004rw, tb_c1004_5, tb_c1004_5n); tb_c1004rw += tb_c1004_5n; memcpy(tb_c1004rw, tb_c1004_6, tb_c1004_6n); tb_c1004rw += tb_c1004_6n; memcpy(tb_c1004rw, tb_c1004_7, tb_c1004_7n); tb_c1004rw += tb_c1004_7n; *tb_c1004rw = 0; (const char *)tb_c1004r; }));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, ({ const char *tb_c1005_0 = v_p; long long tb_c1005_0n = (long long)strlen(tb_c1005_0); const char *tb_c1005_1 = ".len = "; long long tb_c1005_1n = sizeof(".len = ") - 1; const char *tb_c1005_2 = v_l; long long tb_c1005_2n = (long long)strlen(tb_c1005_2); const char *tb_c1005_3 = "->len - "; long long tb_c1005_3n = sizeof("->len - ") - 1; const char *tb_c1005_4 = v_i; long long tb_c1005_4n = (long long)strlen(tb_c1005_4); const char *tb_c1005_5 = " < "; long long tb_c1005_5n = sizeof(" < ") - 1; const char *tb_c1005_6 = v_m; long long tb_c1005_6n = (long long)strlen(tb_c1005_6); const char *tb_c1005_7 = " ? "; long long tb_c1005_7n = sizeof(" ? ") - 1; const char *tb_c1005_8 = v_l; long long tb_c1005_8n = (long long)strlen(tb_c1005_8); const char *tb_c1005_9 = "->len - "; long long tb_c1005_9n = sizeof("->len - ") - 1; const char *tb_c1005_10 = v_i; long long tb_c1005_10n = (long long)strlen(tb_c1005_10); const char *tb_c1005_11 = " : "; long long tb_c1005_11n = sizeof(" : ") - 1; const char *tb_c1005_12 = v_m; long long tb_c1005_12n = (long long)strlen(tb_c1005_12); const char *tb_c1005_13 = ";\n"; long long tb_c1005_13n = sizeof(";\n") - 1; char *tb_c1005r = tb_mem(tb_c1005_0n + tb_c1005_1n + tb_c1005_2n + tb_c1005_3n + tb_c1005_4n + tb_c1005_5n + tb_c1005_6n + tb_c1005_7n + tb_c1005_8n + tb_c1005_9n + tb_c1005_10n + tb_c1005_11n + tb_c1005_12n + tb_c1005_13n + 1), *tb_c1005rw = tb_c1005r; memcpy(tb_c1005rw, tb_c1005_0, tb_c1005_0n); tb_c1005rw += tb_c1005_0n; memcpy(tb_c1005rw, tb_c1005_1, tb_c1005_1n); tb_c1005rw += tb_c1005_1n; memcpy(tb_c1005rw, tb_c1005_2, tb_c1005_2n); tb_c1005rw += tb_c1005_2n; memcpy(tb_c1005rw, tb_c1005_3, tb_c1005_3n); tb_c1005rw += tb_c1005_3n; memcpy(tb_c1005rw, tb_c1005_4, tb_c1005_4n); tb_c1005rw += tb_c1005_4n; memcpy(tb_c1005rw, tb_c1005_5, tb_c1005_5n); tb_c1005rw += tb_c1005_5n; memcpy(tb_c1005rw, tb_c1005_6, tb_c1005_6n); tb_c1005rw += tb_c1005_6n; memcpy(tb_c1005rw, tb_c1005_7, tb_c1005_7n); tb_c1005rw += tb_c1005_7n; memcpy(tb_c1005rw, tb_c1005_8, tb_c1005_8n); tb_c1005rw += tb_c1005_8n; memcpy(tb_c1005rw, tb_c1005_9, tb_c1005_9n); tb_c1005rw += tb_c1005_9n; memcpy(tb_c1005rw, tb_c1005_10, tb_c1005_10n); tb_c1005rw += tb_c1005_10n; memcpy(tb_c1005rw, tb_c1005_11, tb_c1005_11n); tb_c1005rw += tb_c1005_11n; memcpy(tb_c1005rw, tb_c1005_12, tb_c1005_12n); tb_c1005rw += tb_c1005_12n; memcpy(tb_c1005rw, tb_c1005_13, tb_c1005_13n); tb_c1005rw += tb_c1005_13n; *tb_c1005rw = 0; (const char *)tb_c1005r; }));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, ({ const char *tb_c1006_0 = "if ("; long long tb_c1006_0n = sizeof("if (") - 1; const char *tb_c1006_1 = v_i; long long tb_c1006_1n = (long long)strlen(tb_c1006_1); const char *tb_c1006_2 = " + "; long long tb_c1006_2n = sizeof(" + ") - 1; const char *tb_c1006_3 = v_m; long long tb_c1006_3n = (long long)strlen(tb_c1006_3); const char *tb_c1006_4 = " < "; long long tb_c1006_4n = sizeof(" < ") - 1; const char *tb_c1006_5 = v_l; long long tb_c1006_5n = (long long)strlen(tb_c1006_5); const char *tb_c1006_6 = "->len) __builtin_prefetch("; long long tb_c1006_6n = sizeof("->len) __builtin_prefetch(") - 1; const char *tb_c1006_7 = v_p; long long tb_c1006_7n = (long long)strlen(tb_c1006_7); const char *tb_c1006_8 = ".data + "; long long tb_c1006_8n = sizeof(".data + ") - 1; const char *tb_c1006_9 = v_m; long long tb_c1006_9n = (long long)strlen(tb_c1006_9); const char *tb_c1006_10 = " * "; long long tb_c1006_10n = sizeof(" * ") - 1; const char *tb_c1006_11 = v_l; long long tb_c1006_11n = (long long)strlen(tb_c1006_11); const char *tb_c1006_12 = "->size);\n"; long long tb_c1006_12n = sizeof("->size);\n") - 1; char *tb_c1006r = tb_mem(tb_c1006_0n + tb_c1006_1n + tb_c1006_2n + tb_c1006_3n + tb_c1006_4n + tb_c1006_5n + tb_c1006_6n + tb_c1006_7n + tb_c1006_8n + tb_c1006_9n + tb_c1006_10n + tb_c1006_11n + tb_c1006_12n + 1), *tb_c1006rw = tb_c1006r; memcpy(tb_c1006rw, tb_c1006_0, tb_c1006_0n); tb_c1006rw += tb_c1006_0n; memcpy(tb_c1006rw, tb_c1006_1, tb_c1006_1n); tb_c1006rw += tb_c1006_1n; memcpy(tb_c1006rw, tb_c1006_2, tb_c1006_2n); tb_c1006rw += tb_c1006_2n; memcpy(tb_c1006rw, tb_c1006_3, tb_c1006_3n); tb_c1006rw += tb_c1006_3n; memcpy(tb_c1006rw, tb_c1006_4, tb_c1006_4n); tb_c1006rw += tb_c1006_4n; memcpy(tb_c1006rw, tb_c1006_5, tb_c1006_5n); tb_c1006rw += tb_c1006_5n; memcpy(tb_c1006rw, tb_c1006_6, tb_c1006_6n); tb_c1006rw += tb_c1006_6n; memcpy(tb_c1006rw, tb_c1006_7, tb_c1006_7n); tb_c1006rw += tb_c1006_7n; memcpy(tb_c1006rw, tb_c1006_8, tb_c1006_8n); tb_c1006rw += tb_c1006_8n; memcpy(tb_c1006rw, tb_c1006_9, tb_c1006_9n); tb_c1006rw += tb_c1006_9n; memcpy(tb_c1006rw, tb_c1006_10, tb_c1006_10n); tb_c1006rw += tb_c1006_10n; memcpy(tb_c1006rw, tb_c1006_11, tb_c1006_11n); tb_c1006rw += tb_c1006_11n; memcpy(tb_c1006rw, tb_c1006_12, tb_c1006_12n); tb_c1006rw += tb_c1006_12n; *tb_c1006rw = 0; (const char *)tb_c1006r; }));
    f_emit__indent(v_c, (v_depth + 2LL));
    f_emit__put(v_c, ({ const char *tb_c1007_0 = "tb_list * const v_"; long long tb_c1007_0n = sizeof("tb_list * const v_") - 1; const char *tb_c1007_1 = ((tb_o_Node *)tb_nn(v_n, 1064))->q_name; long long tb_c1007_1n = (long long)strlen(tb_c1007_1); const char *tb_c1007_2 = " = &"; long long tb_c1007_2n = sizeof(" = &") - 1; const char *tb_c1007_3 = v_p; long long tb_c1007_3n = (long long)strlen(tb_c1007_3); const char *tb_c1007_4 = ";\n"; long long tb_c1007_4n = sizeof(";\n") - 1; char *tb_c1007r = tb_mem(tb_c1007_0n + tb_c1007_1n + tb_c1007_2n + tb_c1007_3n + tb_c1007_4n + 1), *tb_c1007rw = tb_c1007r; memcpy(tb_c1007rw, tb_c1007_0, tb_c1007_0n); tb_c1007rw += tb_c1007_0n; memcpy(tb_c1007rw, tb_c1007_1, tb_c1007_1n); tb_c1007rw += tb_c1007_1n; memcpy(tb_c1007rw, tb_c1007_2, tb_c1007_2n); tb_c1007rw += tb_c1007_2n; memcpy(tb_c1007rw, tb_c1007_3, tb_c1007_3n); tb_c1007rw += tb_c1007_3n; memcpy(tb_c1007rw, tb_c1007_4, tb_c1007_4n); tb_c1007rw += tb_c1007_4n; *tb_c1007rw = 0; (const char *)tb_c1007r; }));
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1065))->q_kids, 1LL, sizeof(tb_o_Node *), 1065)), (v_depth + 2LL));
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, "}\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c1008_0 = v_l; long long tb_c1008_0n = (long long)strlen(tb_c1008_0); const char *tb_c1008_1 = "->cap = tb_cap"; long long tb_c1008_1n = sizeof("->cap = tb_cap") - 1; const char *tb_c1008_2 = v_id; long long tb_c1008_2n = (long long)strlen(tb_c1008_2); const char *tb_c1008_3 = ";\n"; long long tb_c1008_3n = sizeof(";\n") - 1; char *tb_c1008r = tb_mem(tb_c1008_0n + tb_c1008_1n + tb_c1008_2n + tb_c1008_3n + 1), *tb_c1008rw = tb_c1008r; memcpy(tb_c1008rw, tb_c1008_0, tb_c1008_0n); tb_c1008rw += tb_c1008_0n; memcpy(tb_c1008rw, tb_c1008_1, tb_c1008_1n); tb_c1008rw += tb_c1008_1n; memcpy(tb_c1008rw, tb_c1008_2, tb_c1008_2n); tb_c1008rw += tb_c1008_2n; memcpy(tb_c1008rw, tb_c1008_3, tb_c1008_3n); tb_c1008rw += tb_c1008_3n; *tb_c1008rw = 0; (const char *)tb_c1008r; }));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_background(tb_o_Comp * v_c, tb_o_Node * v_n) {
    { long long tb_v1009 = 1LL; long long *tb_p1009 = &(((tb_o_Comp *)tb_nn(v_c, 1077))->q_label); *tb_p1009 += tb_v1009; }
    const char * v_id = ({ const char *tb_c1010_0 = ""; long long tb_c1010_0n = sizeof("") - 1; char tb_c1010_1d[24]; long long tb_c1010_1n; const char *tb_c1010_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1078))->q_label, tb_c1010_1d, &tb_c1010_1n); char *tb_c1010r = tb_mem(tb_c1010_0n + tb_c1010_1n + 1), *tb_c1010rw = tb_c1010r; memcpy(tb_c1010rw, tb_c1010_0, tb_c1010_0n); tb_c1010rw += tb_c1010_0n; memcpy(tb_c1010rw, tb_c1010_1, tb_c1010_1n); tb_c1010rw += tb_c1010_1n; *tb_c1010rw = 0; (const char *)tb_c1010r; });
    tb_o_Node * v_call = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1079))->q_kids, 0LL, sizeof(tb_o_Node *), 1079));
    tb_o_Node * v_f = ((tb_o_Node *)tb_nn(v_call, 1080))->q_ref;
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1081))->q_kids, 0LL, sizeof(tb_o_Node *), 1081));
    long long v_rt = ((tb_o_Node *)tb_nn(v_f, 1082))->q_ty;
    if ((v_rt < 0LL)) {
        v_rt = 0LL;
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 1085))->q_out;
    tb_list * v_w = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1011 = v_w; tb_list * *tb_p1011 = &(((tb_o_Comp *)tb_nn(v_c, 1087))->q_out); *tb_p1011 = tb_v1011; }
    f_emit__put(v_c, "\ntypedef struct {\n    tb_task tb_t;\n");
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, ({ const char *tb_c1012_0 = "    "; long long tb_c1012_0n = sizeof("    ") - 1; const char *tb_c1012_1 = f_emit__c_type(v_c, v_rt); long long tb_c1012_1n = (long long)strlen(tb_c1012_1); const char *tb_c1012_2 = " tb_r;\n"; long long tb_c1012_2n = sizeof(" tb_r;\n") - 1; char *tb_c1012r = tb_mem(tb_c1012_0n + tb_c1012_1n + tb_c1012_2n + 1), *tb_c1012rw = tb_c1012r; memcpy(tb_c1012rw, tb_c1012_0, tb_c1012_0n); tb_c1012rw += tb_c1012_0n; memcpy(tb_c1012rw, tb_c1012_1, tb_c1012_1n); tb_c1012rw += tb_c1012_1n; memcpy(tb_c1012rw, tb_c1012_2, tb_c1012_2n); tb_c1012rw += tb_c1012_2n; *tb_c1012rw = 0; (const char *)tb_c1012r; }));
    }
    {
        long long tb_end1013 = ((((tb_o_Node *)tb_nn(v_params, 1091))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1013; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1014_0 = "    "; long long tb_c1014_0n = sizeof("    ") - 1; const char *tb_c1014_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1092))->q_kids, v_i, sizeof(tb_o_Node *), 1092)), 1092))->q_ty); long long tb_c1014_1n = (long long)strlen(tb_c1014_1); const char *tb_c1014_2 = " a"; long long tb_c1014_2n = sizeof(" a") - 1; char tb_c1014_3d[24]; long long tb_c1014_3n; const char *tb_c1014_3 = tb_digits(v_i, tb_c1014_3d, &tb_c1014_3n); const char *tb_c1014_4 = ";\n"; long long tb_c1014_4n = sizeof(";\n") - 1; char *tb_c1014r = tb_mem(tb_c1014_0n + tb_c1014_1n + tb_c1014_2n + tb_c1014_3n + tb_c1014_4n + 1), *tb_c1014rw = tb_c1014r; memcpy(tb_c1014rw, tb_c1014_0, tb_c1014_0n); tb_c1014rw += tb_c1014_0n; memcpy(tb_c1014rw, tb_c1014_1, tb_c1014_1n); tb_c1014rw += tb_c1014_1n; memcpy(tb_c1014rw, tb_c1014_2, tb_c1014_2n); tb_c1014rw += tb_c1014_2n; memcpy(tb_c1014rw, tb_c1014_3, tb_c1014_3n); tb_c1014rw += tb_c1014_3n; memcpy(tb_c1014rw, tb_c1014_4, tb_c1014_4n); tb_c1014rw += tb_c1014_4n; *tb_c1014rw = 0; (const char *)tb_c1014r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1015_0 = "} tb_bgjob"; long long tb_c1015_0n = sizeof("} tb_bgjob") - 1; const char *tb_c1015_1 = v_id; long long tb_c1015_1n = (long long)strlen(tb_c1015_1); const char *tb_c1015_2 = ";\n"; long long tb_c1015_2n = sizeof(";\n") - 1; char *tb_c1015r = tb_mem(tb_c1015_0n + tb_c1015_1n + tb_c1015_2n + 1), *tb_c1015rw = tb_c1015r; memcpy(tb_c1015rw, tb_c1015_0, tb_c1015_0n); tb_c1015rw += tb_c1015_0n; memcpy(tb_c1015rw, tb_c1015_1, tb_c1015_1n); tb_c1015rw += tb_c1015_1n; memcpy(tb_c1015rw, tb_c1015_2, tb_c1015_2n); tb_c1015rw += tb_c1015_2n; *tb_c1015rw = 0; (const char *)tb_c1015r; }));
    f_emit__put(v_c, ({ const char *tb_c1016_0 = "static void *tb_bgrun"; long long tb_c1016_0n = sizeof("static void *tb_bgrun") - 1; const char *tb_c1016_1 = v_id; long long tb_c1016_1n = (long long)strlen(tb_c1016_1); const char *tb_c1016_2 = "(void *tb_arg) {\n    tb_bgjob"; long long tb_c1016_2n = sizeof("(void *tb_arg) {\n    tb_bgjob") - 1; const char *tb_c1016_3 = v_id; long long tb_c1016_3n = (long long)strlen(tb_c1016_3); const char *tb_c1016_4 = " *tb_j = tb_arg;\n    "; long long tb_c1016_4n = sizeof(" *tb_j = tb_arg;\n    ") - 1; char *tb_c1016r = tb_mem(tb_c1016_0n + tb_c1016_1n + tb_c1016_2n + tb_c1016_3n + tb_c1016_4n + 1), *tb_c1016rw = tb_c1016r; memcpy(tb_c1016rw, tb_c1016_0, tb_c1016_0n); tb_c1016rw += tb_c1016_0n; memcpy(tb_c1016rw, tb_c1016_1, tb_c1016_1n); tb_c1016rw += tb_c1016_1n; memcpy(tb_c1016rw, tb_c1016_2, tb_c1016_2n); tb_c1016rw += tb_c1016_2n; memcpy(tb_c1016rw, tb_c1016_3, tb_c1016_3n); tb_c1016rw += tb_c1016_3n; memcpy(tb_c1016rw, tb_c1016_4, tb_c1016_4n); tb_c1016rw += tb_c1016_4n; *tb_c1016rw = 0; (const char *)tb_c1016r; }));
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, "tb_j->tb_r = ");
    }
    f_emit__put(v_c, ({ const char *tb_c1017_0 = f_emit__fname(v_f); long long tb_c1017_0n = (long long)strlen(tb_c1017_0); const char *tb_c1017_1 = "("; long long tb_c1017_1n = sizeof("(") - 1; char *tb_c1017r = tb_mem(tb_c1017_0n + tb_c1017_1n + 1), *tb_c1017rw = tb_c1017r; memcpy(tb_c1017rw, tb_c1017_0, tb_c1017_0n); tb_c1017rw += tb_c1017_0n; memcpy(tb_c1017rw, tb_c1017_1, tb_c1017_1n); tb_c1017rw += tb_c1017_1n; *tb_c1017rw = 0; (const char *)tb_c1017r; }));
    {
        long long tb_end1018 = ((((tb_o_Node *)tb_nn(v_params, 1098))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1018; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1019_0 = "tb_j->a"; long long tb_c1019_0n = sizeof("tb_j->a") - 1; char tb_c1019_1d[24]; long long tb_c1019_1n; const char *tb_c1019_1 = tb_digits(v_i, tb_c1019_1d, &tb_c1019_1n); char *tb_c1019r = tb_mem(tb_c1019_0n + tb_c1019_1n + 1), *tb_c1019rw = tb_c1019r; memcpy(tb_c1019rw, tb_c1019_0, tb_c1019_0n); tb_c1019rw += tb_c1019_0n; memcpy(tb_c1019rw, tb_c1019_1, tb_c1019_1n); tb_c1019rw += tb_c1019_1n; *tb_c1019rw = 0; (const char *)tb_c1019r; }));
        }
    }
    f_emit__put(v_c, ");\n    return 0;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1020_0 = "static tb_task *tb_bg"; long long tb_c1020_0n = sizeof("static tb_task *tb_bg") - 1; const char *tb_c1020_1 = v_id; long long tb_c1020_1n = (long long)strlen(tb_c1020_1); const char *tb_c1020_2 = "("; long long tb_c1020_2n = sizeof("(") - 1; char *tb_c1020r = tb_mem(tb_c1020_0n + tb_c1020_1n + tb_c1020_2n + 1), *tb_c1020rw = tb_c1020r; memcpy(tb_c1020rw, tb_c1020_0, tb_c1020_0n); tb_c1020rw += tb_c1020_0n; memcpy(tb_c1020rw, tb_c1020_1, tb_c1020_1n); tb_c1020rw += tb_c1020_1n; memcpy(tb_c1020rw, tb_c1020_2, tb_c1020_2n); tb_c1020rw += tb_c1020_2n; *tb_c1020rw = 0; (const char *)tb_c1020r; }));
    {
        long long tb_end1021 = ((((tb_o_Node *)tb_nn(v_params, 1104))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1021; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1022_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1107))->q_kids, v_i, sizeof(tb_o_Node *), 1107)), 1107))->q_ty); long long tb_c1022_0n = (long long)strlen(tb_c1022_0); const char *tb_c1022_1 = " a"; long long tb_c1022_1n = sizeof(" a") - 1; char tb_c1022_2d[24]; long long tb_c1022_2n; const char *tb_c1022_2 = tb_digits(v_i, tb_c1022_2d, &tb_c1022_2n); char *tb_c1022r = tb_mem(tb_c1022_0n + tb_c1022_1n + tb_c1022_2n + 1), *tb_c1022rw = tb_c1022r; memcpy(tb_c1022rw, tb_c1022_0, tb_c1022_0n); tb_c1022rw += tb_c1022_0n; memcpy(tb_c1022rw, tb_c1022_1, tb_c1022_1n); tb_c1022rw += tb_c1022_1n; memcpy(tb_c1022rw, tb_c1022_2, tb_c1022_2n); tb_c1022rw += tb_c1022_2n; *tb_c1022rw = 0; (const char *)tb_c1022r; }));
        }
    }
    if (((((tb_o_Node *)tb_nn(v_params, 1108))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    f_emit__put(v_c, ({ const char *tb_c1023_0 = ") {\n    tb_bgjob"; long long tb_c1023_0n = sizeof(") {\n    tb_bgjob") - 1; const char *tb_c1023_1 = v_id; long long tb_c1023_1n = (long long)strlen(tb_c1023_1); const char *tb_c1023_2 = " *tb_j = malloc(sizeof(tb_bgjob"; long long tb_c1023_2n = sizeof(" *tb_j = malloc(sizeof(tb_bgjob") - 1; const char *tb_c1023_3 = v_id; long long tb_c1023_3n = (long long)strlen(tb_c1023_3); const char *tb_c1023_4 = "));\n"; long long tb_c1023_4n = sizeof("));\n") - 1; char *tb_c1023r = tb_mem(tb_c1023_0n + tb_c1023_1n + tb_c1023_2n + tb_c1023_3n + tb_c1023_4n + 1), *tb_c1023rw = tb_c1023r; memcpy(tb_c1023rw, tb_c1023_0, tb_c1023_0n); tb_c1023rw += tb_c1023_0n; memcpy(tb_c1023rw, tb_c1023_1, tb_c1023_1n); tb_c1023rw += tb_c1023_1n; memcpy(tb_c1023rw, tb_c1023_2, tb_c1023_2n); tb_c1023rw += tb_c1023_2n; memcpy(tb_c1023rw, tb_c1023_3, tb_c1023_3n); tb_c1023rw += tb_c1023_3n; memcpy(tb_c1023rw, tb_c1023_4, tb_c1023_4n); tb_c1023rw += tb_c1023_4n; *tb_c1023rw = 0; (const char *)tb_c1023r; }));
    f_emit__put(v_c, "    if (tb_j == 0) tb_crash(0, \"не хватило памяти\");\n");
    {
        long long tb_end1024 = ((((tb_o_Node *)tb_nn(v_params, 1112))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1024; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1025_0 = "    tb_j->a"; long long tb_c1025_0n = sizeof("    tb_j->a") - 1; char tb_c1025_1d[24]; long long tb_c1025_1n; const char *tb_c1025_1 = tb_digits(v_i, tb_c1025_1d, &tb_c1025_1n); const char *tb_c1025_2 = " = a"; long long tb_c1025_2n = sizeof(" = a") - 1; char tb_c1025_3d[24]; long long tb_c1025_3n; const char *tb_c1025_3 = tb_digits(v_i, tb_c1025_3d, &tb_c1025_3n); const char *tb_c1025_4 = ";\n"; long long tb_c1025_4n = sizeof(";\n") - 1; char *tb_c1025r = tb_mem(tb_c1025_0n + tb_c1025_1n + tb_c1025_2n + tb_c1025_3n + tb_c1025_4n + 1), *tb_c1025rw = tb_c1025r; memcpy(tb_c1025rw, tb_c1025_0, tb_c1025_0n); tb_c1025rw += tb_c1025_0n; memcpy(tb_c1025rw, tb_c1025_1, tb_c1025_1n); tb_c1025rw += tb_c1025_1n; memcpy(tb_c1025rw, tb_c1025_2, tb_c1025_2n); tb_c1025rw += tb_c1025_2n; memcpy(tb_c1025rw, tb_c1025_3, tb_c1025_3n); tb_c1025rw += tb_c1025_3n; memcpy(tb_c1025rw, tb_c1025_4, tb_c1025_4n); tb_c1025rw += tb_c1025_4n; *tb_c1025rw = 0; (const char *)tb_c1025r; }));
        }
    }
    if ((v_rt != 0LL)) {
        f_emit__put(v_c, "    tb_j->tb_t.res = &tb_j->tb_r;\n");
    } else {
        f_emit__put(v_c, "    tb_j->tb_t.res = 0;\n");
    }
    f_emit__put(v_c, ({ const char *tb_c1026_0 = "    tb_task_start(&tb_j->tb_t, tb_bgrun"; long long tb_c1026_0n = sizeof("    tb_task_start(&tb_j->tb_t, tb_bgrun") - 1; const char *tb_c1026_1 = v_id; long long tb_c1026_1n = (long long)strlen(tb_c1026_1); const char *tb_c1026_2 = ", tb_j);\n    return &tb_j->tb_t;\n}\n"; long long tb_c1026_2n = sizeof(", tb_j);\n    return &tb_j->tb_t;\n}\n") - 1; char *tb_c1026r = tb_mem(tb_c1026_0n + tb_c1026_1n + tb_c1026_2n + 1), *tb_c1026rw = tb_c1026r; memcpy(tb_c1026rw, tb_c1026_0, tb_c1026_0n); tb_c1026rw += tb_c1026_0n; memcpy(tb_c1026rw, tb_c1026_1, tb_c1026_1n); tb_c1026rw += tb_c1026_1n; memcpy(tb_c1026rw, tb_c1026_2, tb_c1026_2n); tb_c1026rw += tb_c1026_2n; *tb_c1026rw = 0; (const char *)tb_c1026r; }));
    { tb_list * tb_v1027 = v_saved; tb_list * *tb_p1027 = &(((tb_o_Comp *)tb_nn(v_c, 1119))->q_out); *tb_p1027 = tb_v1027; }
    { const char * tb_v1028 = tb_join(v_w, ""); *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1120))->q_workers) = tb_v1028; }
    f_emit__put(v_c, ({ const char *tb_c1029_0 = "tb_bg"; long long tb_c1029_0n = sizeof("tb_bg") - 1; const char *tb_c1029_1 = v_id; long long tb_c1029_1n = (long long)strlen(tb_c1029_1); const char *tb_c1029_2 = "("; long long tb_c1029_2n = sizeof("(") - 1; char *tb_c1029r = tb_mem(tb_c1029_0n + tb_c1029_1n + tb_c1029_2n + 1), *tb_c1029rw = tb_c1029r; memcpy(tb_c1029rw, tb_c1029_0, tb_c1029_0n); tb_c1029rw += tb_c1029_0n; memcpy(tb_c1029rw, tb_c1029_1, tb_c1029_1n); tb_c1029rw += tb_c1029_1n; memcpy(tb_c1029rw, tb_c1029_2, tb_c1029_2n); tb_c1029rw += tb_c1029_2n; *tb_c1029rw = 0; (const char *)tb_c1029r; }));
    {
        long long tb_end1030 = ((((tb_o_Node *)tb_nn(v_call, 1122))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1030; v_i++) {
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
    if (((((({ const char *tb_s1031 = v_k; ((unsigned char)tb_s1031[0] == 110 && (unsigned char)tb_s1031[1] == 117 && (unsigned char)tb_s1031[2] == 109 && (unsigned char)tb_s1031[3] == 98 && (unsigned char)tb_s1031[4] == 101 && (unsigned char)tb_s1031[5] == 114 && tb_s1031[6] == 0); }) || ({ const char *tb_s1032 = v_k; ((unsigned char)tb_s1032[0] == 100 && (unsigned char)tb_s1032[1] == 101 && (unsigned char)tb_s1032[2] == 99 && (unsigned char)tb_s1032[3] == 105 && (unsigned char)tb_s1032[4] == 109 && (unsigned char)tb_s1032[5] == 97 && (unsigned char)tb_s1032[6] == 108 && tb_s1032[7] == 0); })) || ({ const char *tb_s1033 = v_k; ((unsigned char)tb_s1033[0] == 115 && (unsigned char)tb_s1033[1] == 116 && (unsigned char)tb_s1033[2] == 114 && (unsigned char)tb_s1033[3] == 105 && (unsigned char)tb_s1033[4] == 110 && (unsigned char)tb_s1033[5] == 103 && tb_s1033[6] == 0); })) || ({ const char *tb_s1034 = v_k; ((unsigned char)tb_s1034[0] == 98 && (unsigned char)tb_s1034[1] == 111 && (unsigned char)tb_s1034[2] == 111 && (unsigned char)tb_s1034[3] == 108 && tb_s1034[4] == 0); })) || ({ const char *tb_s1035 = v_k; ((unsigned char)tb_s1035[0] == 110 && (unsigned char)tb_s1035[1] == 97 && (unsigned char)tb_s1035[2] == 109 && (unsigned char)tb_s1035[3] == 101 && tb_s1035[4] == 0); }))) {
        return 1;
    }
    if ((({ const char *tb_s1036 = v_k; ((unsigned char)tb_s1036[0] == 102 && (unsigned char)tb_s1036[1] == 105 && (unsigned char)tb_s1036[2] == 101 && (unsigned char)tb_s1036[3] == 108 && (unsigned char)tb_s1036[4] == 100 && tb_s1036[5] == 0); }) || ({ const char *tb_s1037 = v_k; ((unsigned char)tb_s1037[0] == 117 && (unsigned char)tb_s1037[1] == 110 && (unsigned char)tb_s1037[2] == 97 && (unsigned char)tb_s1037[3] == 114 && (unsigned char)tb_s1037[4] == 121 && tb_s1037[5] == 0); }))) {
        return f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1134))->q_kids, 0LL, sizeof(tb_o_Node *), 1134)));
    }
    if (({ const char *tb_s1038 = v_k; ((unsigned char)tb_s1038[0] == 98 && (unsigned char)tb_s1038[1] == 105 && (unsigned char)tb_s1038[2] == 110 && (unsigned char)tb_s1038[3] == 111 && (unsigned char)tb_s1038[4] == 112 && tb_s1038[5] == 0); })) {
        if ((({ const char *tb_s1039 = ((tb_o_Node *)tb_nn(v_n, 1136))->q_value; ((unsigned char)tb_s1039[0] == 47 && tb_s1039[1] == 0); }) || ({ const char *tb_s1040 = ((tb_o_Node *)tb_nn(v_n, 1136))->q_value; ((unsigned char)tb_s1040[0] == 37 && tb_s1040[1] == 0); }))) {
            return 0;
        }
        return (f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1138))->q_kids, 0LL, sizeof(tb_o_Node *), 1138))) && f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1138))->q_kids, 1LL, sizeof(tb_o_Node *), 1138))));
    }
    return 0;
    tb_crash(1129, "функция is_pure дошла до конца без return");
    return 0;
}

static void f_emit__emit_together(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    { long long tb_v1041 = 1LL; long long *tb_p1041 = &(((tb_o_Comp *)tb_nn(v_c, 1147))->q_label); *tb_p1041 += tb_v1041; }
    const char * v_id = ({ const char *tb_c1042_0 = ""; long long tb_c1042_0n = sizeof("") - 1; char tb_c1042_1d[24]; long long tb_c1042_1n; const char *tb_c1042_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1148))->q_label, tb_c1042_1d, &tb_c1042_1n); char *tb_c1042r = tb_mem(tb_c1042_0n + tb_c1042_1n + 1), *tb_c1042rw = tb_c1042r; memcpy(tb_c1042rw, tb_c1042_0, tb_c1042_0n); tb_c1042rw += tb_c1042_0n; memcpy(tb_c1042rw, tb_c1042_1, tb_c1042_1n); tb_c1042rw += tb_c1042_1n; *tb_c1042rw = 0; (const char *)tb_c1042r; });
    tb_o_Node * v_block = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1149))->q_kids, 0LL, sizeof(tb_o_Node *), 1149));
    {
        long long tb_end1043 = ((((tb_o_Node *)tb_nn(v_block, 1150))->q_kids)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end1043; v_j++) {
            tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_block, 1151))->q_kids, v_j, sizeof(tb_o_Node *), 1151)), 1151))->q_kids, 0LL, sizeof(tb_o_Node *), 1151));
            if (((({ const char *tb_s1044 = ((tb_o_Node *)tb_nn(v_v, 1152))->q_kind; !((unsigned char)tb_s1044[0] == 105 && (unsigned char)tb_s1044[1] == 110 && (unsigned char)tb_s1044[2] == 100 && (unsigned char)tb_s1044[3] == 101 && (unsigned char)tb_s1044[4] == 120 && tb_s1044[5] == 0); }) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1152))->q_kids, 0LL, sizeof(tb_o_Node *), 1152))))) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1152))->q_kids, 1LL, sizeof(tb_o_Node *), 1152)))))) {
                continue;
            }
            f_emit__indent(v_c, v_depth);
            if (({ const char *tb_s1045 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1155))->q_kids, 0LL, sizeof(tb_o_Node *), 1155)), 1155))->q_ty); ((unsigned char)tb_s1045[0] == 100 && (unsigned char)tb_s1045[1] == 105 && (unsigned char)tb_s1045[2] == 99 && (unsigned char)tb_s1045[3] == 116 && tb_s1045[4] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c1046_0 = "unsigned long long tb_h"; long long tb_c1046_0n = sizeof("unsigned long long tb_h") - 1; const char *tb_c1046_1 = v_id; long long tb_c1046_1n = (long long)strlen(tb_c1046_1); const char *tb_c1046_2 = "_"; long long tb_c1046_2n = sizeof("_") - 1; char tb_c1046_3d[24]; long long tb_c1046_3n; const char *tb_c1046_3 = tb_digits(v_j, tb_c1046_3d, &tb_c1046_3n); const char *tb_c1046_4 = " = tb_dpre1("; long long tb_c1046_4n = sizeof(" = tb_dpre1(") - 1; char *tb_c1046r = tb_mem(tb_c1046_0n + tb_c1046_1n + tb_c1046_2n + tb_c1046_3n + tb_c1046_4n + 1), *tb_c1046rw = tb_c1046r; memcpy(tb_c1046rw, tb_c1046_0, tb_c1046_0n); tb_c1046rw += tb_c1046_0n; memcpy(tb_c1046rw, tb_c1046_1, tb_c1046_1n); tb_c1046rw += tb_c1046_1n; memcpy(tb_c1046rw, tb_c1046_2, tb_c1046_2n); tb_c1046rw += tb_c1046_2n; memcpy(tb_c1046rw, tb_c1046_3, tb_c1046_3n); tb_c1046rw += tb_c1046_3n; memcpy(tb_c1046rw, tb_c1046_4, tb_c1046_4n); tb_c1046rw += tb_c1046_4n; *tb_c1046rw = 0; (const char *)tb_c1046r; }));
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
        long long tb_end1047 = ((((tb_o_Node *)tb_nn(v_block, 1169))->q_kids)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end1047; v_j++) {
            tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_block, 1170))->q_kids, v_j, sizeof(tb_o_Node *), 1170)), 1170))->q_kids, 0LL, sizeof(tb_o_Node *), 1170));
            if (((({ const char *tb_s1048 = ((tb_o_Node *)tb_nn(v_v, 1171))->q_kind; !((unsigned char)tb_s1048[0] == 105 && (unsigned char)tb_s1048[1] == 110 && (unsigned char)tb_s1048[2] == 100 && (unsigned char)tb_s1048[3] == 101 && (unsigned char)tb_s1048[4] == 120 && tb_s1048[5] == 0); }) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1171))->q_kids, 0LL, sizeof(tb_o_Node *), 1171))))) || (!f_emit__is_pure((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1171))->q_kids, 1LL, sizeof(tb_o_Node *), 1171)))))) {
                continue;
            }
            if (({ const char *tb_s1049 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1173))->q_kids, 0LL, sizeof(tb_o_Node *), 1173)), 1173))->q_ty); ((unsigned char)tb_s1049[0] == 100 && (unsigned char)tb_s1049[1] == 105 && (unsigned char)tb_s1049[2] == 99 && (unsigned char)tb_s1049[3] == 116 && tb_s1049[4] == 0); })) {
                f_emit__indent(v_c, v_depth);
                f_emit__put(v_c, "tb_dpre2(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_v, 1176))->q_kids, 0LL, sizeof(tb_o_Node *), 1176)));
                f_emit__put(v_c, ({ const char *tb_c1050_0 = ", tb_h"; long long tb_c1050_0n = sizeof(", tb_h") - 1; const char *tb_c1050_1 = v_id; long long tb_c1050_1n = (long long)strlen(tb_c1050_1); const char *tb_c1050_2 = "_"; long long tb_c1050_2n = sizeof("_") - 1; char tb_c1050_3d[24]; long long tb_c1050_3n; const char *tb_c1050_3 = tb_digits(v_j, tb_c1050_3d, &tb_c1050_3n); const char *tb_c1050_4 = ");\n"; long long tb_c1050_4n = sizeof(");\n") - 1; char *tb_c1050r = tb_mem(tb_c1050_0n + tb_c1050_1n + tb_c1050_2n + tb_c1050_3n + tb_c1050_4n + 1), *tb_c1050rw = tb_c1050r; memcpy(tb_c1050rw, tb_c1050_0, tb_c1050_0n); tb_c1050rw += tb_c1050_0n; memcpy(tb_c1050rw, tb_c1050_1, tb_c1050_1n); tb_c1050rw += tb_c1050_1n; memcpy(tb_c1050rw, tb_c1050_2, tb_c1050_2n); tb_c1050rw += tb_c1050_2n; memcpy(tb_c1050rw, tb_c1050_3, tb_c1050_3n); tb_c1050rw += tb_c1050_3n; memcpy(tb_c1050rw, tb_c1050_4, tb_c1050_4n); tb_c1050rw += tb_c1050_4n; *tb_c1050rw = 0; (const char *)tb_c1050r; }));
            }
        }
    }
    {
        tb_list * tb_l1051 = ((tb_o_Node *)tb_nn(v_block, 1178))->q_kids;
        for (long long tb_i1051 = 0; tb_i1051 < tb_l1051->len; tb_i1051++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l1051->data)[tb_i1051];
            f_emit__emit_statement(v_c, v_s, v_depth);
        }
    }
}

static void f_emit__emit_prefetch(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth, const char * v_list_c, const char * v_i_c, const char * v_elem) {
    {
        long long tb_end1052 = ((((tb_o_Node *)tb_nn(v_n, 1185))->q_pre_bases)->len - 1LL);
        for (long long v_j = 0LL; v_j <= tb_end1052; v_j++) {
            { long long tb_v1053 = 1LL; long long *tb_p1053 = &(((tb_o_Comp *)tb_nn(v_c, 1186))->q_label); *tb_p1053 += tb_v1053; }
            const char * v_f = ({ const char *tb_c1054_0 = "tb_f"; long long tb_c1054_0n = sizeof("tb_f") - 1; char tb_c1054_1d[24]; long long tb_c1054_1n; const char *tb_c1054_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1187))->q_label, tb_c1054_1d, &tb_c1054_1n); char *tb_c1054r = tb_mem(tb_c1054_0n + tb_c1054_1n + 1), *tb_c1054rw = tb_c1054r; memcpy(tb_c1054rw, tb_c1054_0, tb_c1054_0n); tb_c1054rw += tb_c1054_0n; memcpy(tb_c1054rw, tb_c1054_1, tb_c1054_1n); tb_c1054rw += tb_c1054_1n; *tb_c1054rw = 0; (const char *)tb_c1054r; });
            const char * v_plan = v_list_c;
            f_emit__indent(v_c, v_depth);
            if (({ const char *tb_s1055 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1190))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1190)), 1190))->q_kind; ((unsigned char)tb_s1055[0] == 110 && (unsigned char)tb_s1055[1] == 97 && (unsigned char)tb_s1055[2] == 109 && (unsigned char)tb_s1055[3] == 101 && tb_s1055[4] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c1056_0 = "{ tb_list *tb_q"; long long tb_c1056_0n = sizeof("{ tb_list *tb_q") - 1; char tb_c1056_1d[24]; long long tb_c1056_1n; const char *tb_c1056_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1191))->q_label, tb_c1056_1d, &tb_c1056_1n); const char *tb_c1056_2 = " = "; long long tb_c1056_2n = sizeof(" = ") - 1; char *tb_c1056r = tb_mem(tb_c1056_0n + tb_c1056_1n + tb_c1056_2n + 1), *tb_c1056rw = tb_c1056r; memcpy(tb_c1056rw, tb_c1056_0, tb_c1056_0n); tb_c1056rw += tb_c1056_0n; memcpy(tb_c1056rw, tb_c1056_1, tb_c1056_1n); tb_c1056rw += tb_c1056_1n; memcpy(tb_c1056rw, tb_c1056_2, tb_c1056_2n); tb_c1056rw += tb_c1056_2n; *tb_c1056rw = 0; (const char *)tb_c1056r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1192))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1192)));
                f_emit__put(v_c, "; ");
                v_plan = ({ const char *tb_c1057_0 = "tb_q"; long long tb_c1057_0n = sizeof("tb_q") - 1; char tb_c1057_1d[24]; long long tb_c1057_1n; const char *tb_c1057_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1194))->q_label, tb_c1057_1d, &tb_c1057_1n); char *tb_c1057r = tb_mem(tb_c1057_0n + tb_c1057_1n + 1), *tb_c1057rw = tb_c1057r; memcpy(tb_c1057rw, tb_c1057_0, tb_c1057_0n); tb_c1057rw += tb_c1057_0n; memcpy(tb_c1057rw, tb_c1057_1, tb_c1057_1n); tb_c1057rw += tb_c1057_1n; *tb_c1057rw = 0; (const char *)tb_c1057r; });
            } else {
                f_emit__put(v_c, "{ ");
            }
            f_emit__put(v_c, ({ const char *tb_c1058_0 = "if ((unsigned long long)("; long long tb_c1058_0n = sizeof("if ((unsigned long long)(") - 1; const char *tb_c1058_1 = v_i_c; long long tb_c1058_1n = (long long)strlen(tb_c1058_1); const char *tb_c1058_2 = " + 16) < (unsigned long long)"; long long tb_c1058_2n = sizeof(" + 16) < (unsigned long long)") - 1; const char *tb_c1058_3 = v_plan; long long tb_c1058_3n = (long long)strlen(tb_c1058_3); const char *tb_c1058_4 = "->len) { "; long long tb_c1058_4n = sizeof("->len) { ") - 1; char *tb_c1058r = tb_mem(tb_c1058_0n + tb_c1058_1n + tb_c1058_2n + tb_c1058_3n + tb_c1058_4n + 1), *tb_c1058rw = tb_c1058r; memcpy(tb_c1058rw, tb_c1058_0, tb_c1058_0n); tb_c1058rw += tb_c1058_0n; memcpy(tb_c1058rw, tb_c1058_1, tb_c1058_1n); tb_c1058rw += tb_c1058_1n; memcpy(tb_c1058rw, tb_c1058_2, tb_c1058_2n); tb_c1058rw += tb_c1058_2n; memcpy(tb_c1058rw, tb_c1058_3, tb_c1058_3n); tb_c1058rw += tb_c1058_3n; memcpy(tb_c1058rw, tb_c1058_4, tb_c1058_4n); tb_c1058rw += tb_c1058_4n; *tb_c1058rw = 0; (const char *)tb_c1058r; }));
            const char * v_el = v_elem;
            if (({ const char *tb_s1059 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1199))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1199)), 1199))->q_kind; ((unsigned char)tb_s1059[0] == 110 && (unsigned char)tb_s1059[1] == 97 && (unsigned char)tb_s1059[2] == 109 && (unsigned char)tb_s1059[3] == 101 && tb_s1059[4] == 0); })) {
                v_el = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1200))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1200))->q_pre_idx, v_j, sizeof(tb_o_Node *), 1200)), 1200))->q_ty, sizeof(tb_o_Type *), 1200)), 1200))->q_elem);
            }
            f_emit__put(v_c, ({ const char *tb_c1060_0 = v_el; long long tb_c1060_0n = (long long)strlen(tb_c1060_0); const char *tb_c1060_1 = " "; long long tb_c1060_1n = sizeof(" ") - 1; const char *tb_c1060_2 = v_f; long long tb_c1060_2n = (long long)strlen(tb_c1060_2); const char *tb_c1060_3 = " = (("; long long tb_c1060_3n = sizeof(" = ((") - 1; const char *tb_c1060_4 = v_el; long long tb_c1060_4n = (long long)strlen(tb_c1060_4); const char *tb_c1060_5 = " *)"; long long tb_c1060_5n = sizeof(" *)") - 1; const char *tb_c1060_6 = v_plan; long long tb_c1060_6n = (long long)strlen(tb_c1060_6); const char *tb_c1060_7 = "->data)["; long long tb_c1060_7n = sizeof("->data)[") - 1; const char *tb_c1060_8 = v_i_c; long long tb_c1060_8n = (long long)strlen(tb_c1060_8); const char *tb_c1060_9 = " + 16]; "; long long tb_c1060_9n = sizeof(" + 16]; ") - 1; char *tb_c1060r = tb_mem(tb_c1060_0n + tb_c1060_1n + tb_c1060_2n + tb_c1060_3n + tb_c1060_4n + tb_c1060_5n + tb_c1060_6n + tb_c1060_7n + tb_c1060_8n + tb_c1060_9n + 1), *tb_c1060rw = tb_c1060r; memcpy(tb_c1060rw, tb_c1060_0, tb_c1060_0n); tb_c1060rw += tb_c1060_0n; memcpy(tb_c1060rw, tb_c1060_1, tb_c1060_1n); tb_c1060rw += tb_c1060_1n; memcpy(tb_c1060rw, tb_c1060_2, tb_c1060_2n); tb_c1060rw += tb_c1060_2n; memcpy(tb_c1060rw, tb_c1060_3, tb_c1060_3n); tb_c1060rw += tb_c1060_3n; memcpy(tb_c1060rw, tb_c1060_4, tb_c1060_4n); tb_c1060rw += tb_c1060_4n; memcpy(tb_c1060rw, tb_c1060_5, tb_c1060_5n); tb_c1060rw += tb_c1060_5n; memcpy(tb_c1060rw, tb_c1060_6, tb_c1060_6n); tb_c1060rw += tb_c1060_6n; memcpy(tb_c1060rw, tb_c1060_7, tb_c1060_7n); tb_c1060rw += tb_c1060_7n; memcpy(tb_c1060rw, tb_c1060_8, tb_c1060_8n); tb_c1060rw += tb_c1060_8n; memcpy(tb_c1060rw, tb_c1060_9, tb_c1060_9n); tb_c1060rw += tb_c1060_9n; *tb_c1060rw = 0; (const char *)tb_c1060r; }));
            tb_o_Node * v_base = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1202))->q_pre_bases, v_j, sizeof(tb_o_Node *), 1202));
            if (({ const char *tb_s1061 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_base, 1203))->q_ty); ((unsigned char)tb_s1061[0] == 100 && (unsigned char)tb_s1061[1] == 105 && (unsigned char)tb_s1061[2] == 99 && (unsigned char)tb_s1061[3] == 116 && tb_s1061[4] == 0); })) {
                f_emit__put(v_c, "(void)tb_dpre1(");
                f_emit__emit_expr(v_c, v_base);
                f_emit__put(v_c, ({ const char *tb_c1062_0 = ", &"; long long tb_c1062_0n = sizeof(", &") - 1; const char *tb_c1062_1 = v_f; long long tb_c1062_1n = (long long)strlen(tb_c1062_1); const char *tb_c1062_2 = "); } }\n"; long long tb_c1062_2n = sizeof("); } }\n") - 1; char *tb_c1062r = tb_mem(tb_c1062_0n + tb_c1062_1n + tb_c1062_2n + 1), *tb_c1062rw = tb_c1062r; memcpy(tb_c1062rw, tb_c1062_0, tb_c1062_0n); tb_c1062rw += tb_c1062_0n; memcpy(tb_c1062rw, tb_c1062_1, tb_c1062_1n); tb_c1062rw += tb_c1062_1n; memcpy(tb_c1062rw, tb_c1062_2, tb_c1062_2n); tb_c1062rw += tb_c1062_2n; *tb_c1062rw = 0; (const char *)tb_c1062r; }));
            } else {
                f_emit__put(v_c, "tb_lpre(");
                f_emit__emit_expr(v_c, v_base);
                f_emit__put(v_c, ({ const char *tb_c1063_0 = ", "; long long tb_c1063_0n = sizeof(", ") - 1; const char *tb_c1063_1 = v_f; long long tb_c1063_1n = (long long)strlen(tb_c1063_1); const char *tb_c1063_2 = "); } }\n"; long long tb_c1063_2n = sizeof("); } }\n") - 1; char *tb_c1063r = tb_mem(tb_c1063_0n + tb_c1063_1n + tb_c1063_2n + 1), *tb_c1063rw = tb_c1063r; memcpy(tb_c1063rw, tb_c1063_0, tb_c1063_0n); tb_c1063rw += tb_c1063_0n; memcpy(tb_c1063rw, tb_c1063_1, tb_c1063_1n); tb_c1063rw += tb_c1063_1n; memcpy(tb_c1063rw, tb_c1063_2, tb_c1063_2n); tb_c1063rw += tb_c1063_2n; *tb_c1063rw = 0; (const char *)tb_c1063r; }));
            }
        }
    }
}

static _Bool f_emit__short_literal(tb_o_Node * v_n) {
    if (((({ const char *tb_s1064 = ((tb_o_Node *)tb_nn(v_n, 1214))->q_kind; !((unsigned char)tb_s1064[0] == 115 && (unsigned char)tb_s1064[1] == 116 && (unsigned char)tb_s1064[2] == 114 && (unsigned char)tb_s1064[3] == 105 && (unsigned char)tb_s1064[4] == 110 && (unsigned char)tb_s1064[5] == 103 && tb_s1064[6] == 0); }) || (tb_tlen(((tb_o_Node *)tb_nn(v_n, 1214))->q_value) < 2LL)) || (tb_tlen(((tb_o_Node *)tb_nn(v_n, 1214))->q_value) > 10LL))) {
        return 0;
    }
    {
        const char * tb_l1065 = ((tb_o_Node *)tb_nn(v_n, 1216))->q_value;
        for (long long tb_i1065 = 0; tb_l1065[tb_i1065] != 0; tb_i1065++) {
            const char * const v_ch = tb_chr((unsigned char)tb_l1065[tb_i1065]);
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
    { long long tb_v1066 = 1LL; long long *tb_p1066 = &(((tb_o_Comp *)tb_nn(v_c, 1225))->q_label); *tb_p1066 += tb_v1066; }
    const char * v_id = ({ const char *tb_c1067_0 = ""; long long tb_c1067_0n = sizeof("") - 1; char tb_c1067_1d[24]; long long tb_c1067_1n; const char *tb_c1067_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1226))->q_label, tb_c1067_1d, &tb_c1067_1n); char *tb_c1067r = tb_mem(tb_c1067_0n + tb_c1067_1n + 1), *tb_c1067rw = tb_c1067r; memcpy(tb_c1067rw, tb_c1067_0, tb_c1067_0n); tb_c1067rw += tb_c1067_0n; memcpy(tb_c1067rw, tb_c1067_1, tb_c1067_1n); tb_c1067rw += tb_c1067_1n; *tb_c1067rw = 0; (const char *)tb_c1067r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "{\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c1068_0 = "long long tb_end"; long long tb_c1068_0n = sizeof("long long tb_end") - 1; const char *tb_c1068_1 = v_id; long long tb_c1068_1n = (long long)strlen(tb_c1068_1); const char *tb_c1068_2 = " = "; long long tb_c1068_2n = sizeof(" = ") - 1; char *tb_c1068r = tb_mem(tb_c1068_0n + tb_c1068_1n + tb_c1068_2n + 1), *tb_c1068rw = tb_c1068r; memcpy(tb_c1068rw, tb_c1068_0, tb_c1068_0n); tb_c1068rw += tb_c1068_0n; memcpy(tb_c1068rw, tb_c1068_1, tb_c1068_1n); tb_c1068rw += tb_c1068_1n; memcpy(tb_c1068rw, tb_c1068_2, tb_c1068_2n); tb_c1068rw += tb_c1068_2n; *tb_c1068rw = 0; (const char *)tb_c1068r; }));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1231))->q_kids, 1LL, sizeof(tb_o_Node *), 1231)));
    f_emit__put(v_c, ";\n");
    f_emit__indent(v_c, (v_depth + 1LL));
    f_emit__put(v_c, ({ const char *tb_c1069_0 = "for (long long v_"; long long tb_c1069_0n = sizeof("for (long long v_") - 1; const char *tb_c1069_1 = ((tb_o_Node *)tb_nn(v_n, 1234))->q_name; long long tb_c1069_1n = (long long)strlen(tb_c1069_1); const char *tb_c1069_2 = " = "; long long tb_c1069_2n = sizeof(" = ") - 1; char *tb_c1069r = tb_mem(tb_c1069_0n + tb_c1069_1n + tb_c1069_2n + 1), *tb_c1069rw = tb_c1069r; memcpy(tb_c1069rw, tb_c1069_0, tb_c1069_0n); tb_c1069rw += tb_c1069_0n; memcpy(tb_c1069rw, tb_c1069_1, tb_c1069_1n); tb_c1069rw += tb_c1069_1n; memcpy(tb_c1069rw, tb_c1069_2, tb_c1069_2n); tb_c1069rw += tb_c1069_2n; *tb_c1069rw = 0; (const char *)tb_c1069r; }));
    f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1235))->q_kids, 0LL, sizeof(tb_o_Node *), 1235)));
    f_emit__put(v_c, ({ const char *tb_c1070_0 = "; v_"; long long tb_c1070_0n = sizeof("; v_") - 1; const char *tb_c1070_1 = ((tb_o_Node *)tb_nn(v_n, 1236))->q_name; long long tb_c1070_1n = (long long)strlen(tb_c1070_1); const char *tb_c1070_2 = " <= tb_end"; long long tb_c1070_2n = sizeof(" <= tb_end") - 1; const char *tb_c1070_3 = v_id; long long tb_c1070_3n = (long long)strlen(tb_c1070_3); const char *tb_c1070_4 = "; v_"; long long tb_c1070_4n = sizeof("; v_") - 1; const char *tb_c1070_5 = ((tb_o_Node *)tb_nn(v_n, 1236))->q_name; long long tb_c1070_5n = (long long)strlen(tb_c1070_5); const char *tb_c1070_6 = "++) {\n"; long long tb_c1070_6n = sizeof("++) {\n") - 1; char *tb_c1070r = tb_mem(tb_c1070_0n + tb_c1070_1n + tb_c1070_2n + tb_c1070_3n + tb_c1070_4n + tb_c1070_5n + tb_c1070_6n + 1), *tb_c1070rw = tb_c1070r; memcpy(tb_c1070rw, tb_c1070_0, tb_c1070_0n); tb_c1070rw += tb_c1070_0n; memcpy(tb_c1070rw, tb_c1070_1, tb_c1070_1n); tb_c1070rw += tb_c1070_1n; memcpy(tb_c1070rw, tb_c1070_2, tb_c1070_2n); tb_c1070rw += tb_c1070_2n; memcpy(tb_c1070rw, tb_c1070_3, tb_c1070_3n); tb_c1070rw += tb_c1070_3n; memcpy(tb_c1070rw, tb_c1070_4, tb_c1070_4n); tb_c1070rw += tb_c1070_4n; memcpy(tb_c1070rw, tb_c1070_5, tb_c1070_5n); tb_c1070rw += tb_c1070_5n; memcpy(tb_c1070rw, tb_c1070_6, tb_c1070_6n); tb_c1070rw += tb_c1070_6n; *tb_c1070rw = 0; (const char *)tb_c1070r; }));
    f_emit__emit_prefetch(v_c, v_n, (v_depth + 2LL), "", ({ const char *tb_c1071_0 = "v_"; long long tb_c1071_0n = sizeof("v_") - 1; const char *tb_c1071_1 = ((tb_o_Node *)tb_nn(v_n, 1237))->q_name; long long tb_c1071_1n = (long long)strlen(tb_c1071_1); char *tb_c1071r = tb_mem(tb_c1071_0n + tb_c1071_1n + 1), *tb_c1071rw = tb_c1071r; memcpy(tb_c1071rw, tb_c1071_0, tb_c1071_0n); tb_c1071rw += tb_c1071_0n; memcpy(tb_c1071rw, tb_c1071_1, tb_c1071_1n); tb_c1071rw += tb_c1071_1n; *tb_c1071rw = 0; (const char *)tb_c1071r; }), "long long");
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
        f_emit__put(v_c, ({ const char *tb_c1072_0 = "return v_"; long long tb_c1072_0n = sizeof("return v_") - 1; const char *tb_c1072_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1249))->q_kids, 0LL, sizeof(tb_o_Node *), 1249)), 1249))->q_name; long long tb_c1072_1n = (long long)strlen(tb_c1072_1); const char *tb_c1072_2 = ";\n"; long long tb_c1072_2n = sizeof(";\n") - 1; char *tb_c1072r = tb_mem(tb_c1072_0n + tb_c1072_1n + tb_c1072_2n + 1), *tb_c1072rw = tb_c1072r; memcpy(tb_c1072rw, tb_c1072_0, tb_c1072_0n); tb_c1072rw += tb_c1072_0n; memcpy(tb_c1072rw, tb_c1072_1, tb_c1072_1n); tb_c1072rw += tb_c1072_1n; memcpy(tb_c1072rw, tb_c1072_2, tb_c1072_2n); tb_c1072rw += tb_c1072_2n; *tb_c1072rw = 0; (const char *)tb_c1072r; }));
        return;
    }
    f_emit__put(v_c, ({ const char *tb_c1073_0 = "return ("; long long tb_c1073_0n = sizeof("return (") - 1; const char *tb_c1073_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1251))->q_cur_func, 1251))->q_ty); long long tb_c1073_1n = (long long)strlen(tb_c1073_1); const char *tb_c1073_2 = "){"; long long tb_c1073_2n = sizeof("){") - 1; char *tb_c1073r = tb_mem(tb_c1073_0n + tb_c1073_1n + tb_c1073_2n + 1), *tb_c1073rw = tb_c1073r; memcpy(tb_c1073rw, tb_c1073_0, tb_c1073_0n); tb_c1073rw += tb_c1073_0n; memcpy(tb_c1073rw, tb_c1073_1, tb_c1073_1n); tb_c1073rw += tb_c1073_1n; memcpy(tb_c1073rw, tb_c1073_2, tb_c1073_2n); tb_c1073rw += tb_c1073_2n; *tb_c1073rw = 0; (const char *)tb_c1073r; }));
    {
        long long tb_end1074 = ((((tb_o_Node *)tb_nn(v_outputs, 1252))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1074; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1075_0 = "v_"; long long tb_c1075_0n = sizeof("v_") - 1; const char *tb_c1075_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_outputs, 1255))->q_kids, v_i, sizeof(tb_o_Node *), 1255)), 1255))->q_name; long long tb_c1075_1n = (long long)strlen(tb_c1075_1); char *tb_c1075r = tb_mem(tb_c1075_0n + tb_c1075_1n + 1), *tb_c1075rw = tb_c1075r; memcpy(tb_c1075rw, tb_c1075_0, tb_c1075_0n); tb_c1075rw += tb_c1075_0n; memcpy(tb_c1075rw, tb_c1075_1, tb_c1075_1n); tb_c1075rw += tb_c1075_1n; *tb_c1075rw = 0; (const char *)tb_c1075r; }));
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
    if ((((({ const char *tb_s1076 = ((tb_o_Node *)tb_nn(v_e, 1264))->q_kind; !((unsigned char)tb_s1076[0] == 109 && (unsigned char)tb_s1076[1] == 101 && (unsigned char)tb_s1076[2] == 116 && (unsigned char)tb_s1076[3] == 104 && (unsigned char)tb_s1076[4] == 111 && (unsigned char)tb_s1076[5] == 100 && tb_s1076[6] == 0); }) || ({ const char *tb_s1077 = ((tb_o_Node *)tb_nn(v_e, 1264))->q_name; !((unsigned char)tb_s1077[0] == 97 && (unsigned char)tb_s1077[1] == 100 && (unsigned char)tb_s1077[2] == 100 && tb_s1077[3] == 0); })) || ({ const char *tb_s1078 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1264))->q_kids, 0LL, sizeof(tb_o_Node *), 1264)), 1264))->q_kind; !((unsigned char)tb_s1078[0] == 110 && (unsigned char)tb_s1078[1] == 97 && (unsigned char)tb_s1078[2] == 109 && (unsigned char)tb_s1078[3] == 101 && tb_s1078[4] == 0); })) || f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1264))->q_kids, 0LL, sizeof(tb_o_Node *), 1264)), 1264))->q_ty))) {
        return 0;
    }
    tb_o_Node * v_v = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1266))->q_kids, 1LL, sizeof(tb_o_Node *), 1266)), 1266))->q_kids, 0LL, sizeof(tb_o_Node *), 1266));
    const char * v_k = ((tb_o_Node *)tb_nn(v_v, 1267))->q_kind;
    return (((({ const char *tb_s1079 = v_k; ((unsigned char)tb_s1079[0] == 110 && (unsigned char)tb_s1079[1] == 117 && (unsigned char)tb_s1079[2] == 109 && (unsigned char)tb_s1079[3] == 98 && (unsigned char)tb_s1079[4] == 101 && (unsigned char)tb_s1079[5] == 114 && tb_s1079[6] == 0); }) || ({ const char *tb_s1080 = v_k; ((unsigned char)tb_s1080[0] == 100 && (unsigned char)tb_s1080[1] == 101 && (unsigned char)tb_s1080[2] == 99 && (unsigned char)tb_s1080[3] == 105 && (unsigned char)tb_s1080[4] == 109 && (unsigned char)tb_s1080[5] == 97 && (unsigned char)tb_s1080[6] == 108 && tb_s1080[7] == 0); })) || ({ const char *tb_s1081 = v_k; ((unsigned char)tb_s1081[0] == 115 && (unsigned char)tb_s1081[1] == 116 && (unsigned char)tb_s1081[2] == 114 && (unsigned char)tb_s1081[3] == 105 && (unsigned char)tb_s1081[4] == 110 && (unsigned char)tb_s1081[5] == 103 && tb_s1081[6] == 0); })) || ({ const char *tb_s1082 = v_k; ((unsigned char)tb_s1082[0] == 98 && (unsigned char)tb_s1082[1] == 111 && (unsigned char)tb_s1082[2] == 111 && (unsigned char)tb_s1082[3] == 108 && tb_s1082[4] == 0); }));
    tb_crash(1259, "функция fill_form дошла до конца без return");
    return 0;
}

static void f_emit__emit_statement(tb_o_Comp * v_c, tb_o_Node * v_n, long long v_depth) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1271))->q_kind;
    if (({ const char *tb_s1083 = v_k; ((unsigned char)tb_s1083[0] == 97 && (unsigned char)tb_s1083[1] == 115 && (unsigned char)tb_s1083[2] == 115 && (unsigned char)tb_s1083[3] == 105 && (unsigned char)tb_s1083[4] == 103 && (unsigned char)tb_s1083[5] == 110 && tb_s1083[6] == 0); })) {
        f_emit__emit_assign(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1084 = v_k; ((unsigned char)tb_s1084[0] == 115 && (unsigned char)tb_s1084[1] == 101 && (unsigned char)tb_s1084[2] == 116 && tb_s1084[3] == 0); })) {
        f_emit__emit_set(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1085 = v_k; ((unsigned char)tb_s1085[0] == 108 && (unsigned char)tb_s1085[1] == 105 && (unsigned char)tb_s1085[2] == 110 && (unsigned char)tb_s1085[3] == 107 && (unsigned char)tb_s1085[4] == 100 && (unsigned char)tb_s1085[5] == 101 && (unsigned char)tb_s1085[6] == 99 && (unsigned char)tb_s1085[7] == 108 && tb_s1085[8] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, ({ const char *tb_c1086_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_n, 1278))->q_ty); long long tb_c1086_0n = (long long)strlen(tb_c1086_0); const char *tb_c1086_1 = " *v_"; long long tb_c1086_1n = sizeof(" *v_") - 1; const char *tb_c1086_2 = ((tb_o_Node *)tb_nn(v_n, 1278))->q_name; long long tb_c1086_2n = (long long)strlen(tb_c1086_2); const char *tb_c1086_3 = " = &("; long long tb_c1086_3n = sizeof(" = &(") - 1; char *tb_c1086r = tb_mem(tb_c1086_0n + tb_c1086_1n + tb_c1086_2n + tb_c1086_3n + 1), *tb_c1086rw = tb_c1086r; memcpy(tb_c1086rw, tb_c1086_0, tb_c1086_0n); tb_c1086rw += tb_c1086_0n; memcpy(tb_c1086rw, tb_c1086_1, tb_c1086_1n); tb_c1086rw += tb_c1086_1n; memcpy(tb_c1086rw, tb_c1086_2, tb_c1086_2n); tb_c1086rw += tb_c1086_2n; memcpy(tb_c1086rw, tb_c1086_3, tb_c1086_3n); tb_c1086rw += tb_c1086_3n; *tb_c1086rw = 0; (const char *)tb_c1086r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1279))->q_kids, 0LL, sizeof(tb_o_Node *), 1279)));
        f_emit__put(v_c, ");\n");
    } else if (({ const char *tb_s1087 = v_k; ((unsigned char)tb_s1087[0] == 109 && (unsigned char)tb_s1087[1] == 117 && (unsigned char)tb_s1087[2] == 108 && (unsigned char)tb_s1087[3] == 116 && (unsigned char)tb_s1087[4] == 105 && tb_s1087[5] == 0); })) {
        f_emit__emit_multi(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1088 = v_k; ((unsigned char)tb_s1088[0] == 105 && (unsigned char)tb_s1088[1] == 102 && tb_s1088[2] == 0); })) {
        f_emit__emit_if(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1089 = v_k; ((unsigned char)tb_s1089[0] == 101 && (unsigned char)tb_s1089[1] == 97 && (unsigned char)tb_s1089[2] == 99 && (unsigned char)tb_s1089[3] == 104 && tb_s1089[4] == 0); })) {
        f_emit__emit_each(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1090 = v_k; ((unsigned char)tb_s1090[0] == 99 && (unsigned char)tb_s1090[1] == 111 && (unsigned char)tb_s1090[2] == 117 && (unsigned char)tb_s1090[3] == 110 && (unsigned char)tb_s1090[4] == 116 && tb_s1090[5] == 0); })) {
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
            f_emit__put(v_c, ({ const char *tb_c1091_0 = ", &("; long long tb_c1091_0n = sizeof(", &(") - 1; const char *tb_c1091_1 = f_emit__c_type(v_c, v_elem); long long tb_c1091_1n = (long long)strlen(tb_c1091_1); const char *tb_c1091_2 = "){"; long long tb_c1091_2n = sizeof("){") - 1; char *tb_c1091r = tb_mem(tb_c1091_0n + tb_c1091_1n + tb_c1091_2n + 1), *tb_c1091rw = tb_c1091r; memcpy(tb_c1091rw, tb_c1091_0, tb_c1091_0n); tb_c1091rw += tb_c1091_0n; memcpy(tb_c1091rw, tb_c1091_1, tb_c1091_1n); tb_c1091rw += tb_c1091_1n; memcpy(tb_c1091rw, tb_c1091_2, tb_c1091_2n); tb_c1091rw += tb_c1091_2n; *tb_c1091rw = 0; (const char *)tb_c1091r; }));
            f_emit__emit_as(v_c, v_value, v_elem);
            f_emit__put(v_c, "});\n");
        }
    } else if ((strcmp(v_k, "repeat_times") == 0)) {
        { long long tb_v1092 = 1LL; long long *tb_p1092 = &(((tb_o_Comp *)tb_nn(v_c, 1313))->q_label); *tb_p1092 += tb_v1092; }
        const char * v_id = ({ const char *tb_c1093_0 = ""; long long tb_c1093_0n = sizeof("") - 1; char tb_c1093_1d[24]; long long tb_c1093_1n; const char *tb_c1093_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1314))->q_label, tb_c1093_1d, &tb_c1093_1n); char *tb_c1093r = tb_mem(tb_c1093_0n + tb_c1093_1n + 1), *tb_c1093rw = tb_c1093r; memcpy(tb_c1093rw, tb_c1093_0, tb_c1093_0n); tb_c1093rw += tb_c1093_0n; memcpy(tb_c1093rw, tb_c1093_1, tb_c1093_1n); tb_c1093rw += tb_c1093_1n; *tb_c1093rw = 0; (const char *)tb_c1093r; });
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, ({ const char *tb_c1094_0 = "for (long long tb_r"; long long tb_c1094_0n = sizeof("for (long long tb_r") - 1; const char *tb_c1094_1 = v_id; long long tb_c1094_1n = (long long)strlen(tb_c1094_1); const char *tb_c1094_2 = " = 0, tb_n"; long long tb_c1094_2n = sizeof(" = 0, tb_n") - 1; const char *tb_c1094_3 = v_id; long long tb_c1094_3n = (long long)strlen(tb_c1094_3); const char *tb_c1094_4 = " = "; long long tb_c1094_4n = sizeof(" = ") - 1; char *tb_c1094r = tb_mem(tb_c1094_0n + tb_c1094_1n + tb_c1094_2n + tb_c1094_3n + tb_c1094_4n + 1), *tb_c1094rw = tb_c1094r; memcpy(tb_c1094rw, tb_c1094_0, tb_c1094_0n); tb_c1094rw += tb_c1094_0n; memcpy(tb_c1094rw, tb_c1094_1, tb_c1094_1n); tb_c1094rw += tb_c1094_1n; memcpy(tb_c1094rw, tb_c1094_2, tb_c1094_2n); tb_c1094rw += tb_c1094_2n; memcpy(tb_c1094rw, tb_c1094_3, tb_c1094_3n); tb_c1094rw += tb_c1094_3n; memcpy(tb_c1094rw, tb_c1094_4, tb_c1094_4n); tb_c1094rw += tb_c1094_4n; *tb_c1094rw = 0; (const char *)tb_c1094r; }));
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1317))->q_kids, 0LL, sizeof(tb_o_Node *), 1317)));
        f_emit__put(v_c, ({ const char *tb_c1095_0 = "; tb_r"; long long tb_c1095_0n = sizeof("; tb_r") - 1; const char *tb_c1095_1 = v_id; long long tb_c1095_1n = (long long)strlen(tb_c1095_1); const char *tb_c1095_2 = " < tb_n"; long long tb_c1095_2n = sizeof(" < tb_n") - 1; const char *tb_c1095_3 = v_id; long long tb_c1095_3n = (long long)strlen(tb_c1095_3); const char *tb_c1095_4 = "; tb_r"; long long tb_c1095_4n = sizeof("; tb_r") - 1; const char *tb_c1095_5 = v_id; long long tb_c1095_5n = (long long)strlen(tb_c1095_5); const char *tb_c1095_6 = "++) {\n"; long long tb_c1095_6n = sizeof("++) {\n") - 1; char *tb_c1095r = tb_mem(tb_c1095_0n + tb_c1095_1n + tb_c1095_2n + tb_c1095_3n + tb_c1095_4n + tb_c1095_5n + tb_c1095_6n + 1), *tb_c1095rw = tb_c1095r; memcpy(tb_c1095rw, tb_c1095_0, tb_c1095_0n); tb_c1095rw += tb_c1095_0n; memcpy(tb_c1095rw, tb_c1095_1, tb_c1095_1n); tb_c1095rw += tb_c1095_1n; memcpy(tb_c1095rw, tb_c1095_2, tb_c1095_2n); tb_c1095rw += tb_c1095_2n; memcpy(tb_c1095rw, tb_c1095_3, tb_c1095_3n); tb_c1095rw += tb_c1095_3n; memcpy(tb_c1095rw, tb_c1095_4, tb_c1095_4n); tb_c1095rw += tb_c1095_4n; memcpy(tb_c1095rw, tb_c1095_5, tb_c1095_5n); tb_c1095rw += tb_c1095_5n; memcpy(tb_c1095rw, tb_c1095_6, tb_c1095_6n); tb_c1095rw += tb_c1095_6n; *tb_c1095rw = 0; (const char *)tb_c1095r; }));
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
    } else if (({ const char *tb_s1096 = v_k; ((unsigned char)tb_s1096[0] == 116 && (unsigned char)tb_s1096[1] == 111 && (unsigned char)tb_s1096[2] == 103 && (unsigned char)tb_s1096[3] == 101 && (unsigned char)tb_s1096[4] == 116 && (unsigned char)tb_s1096[5] == 104 && (unsigned char)tb_s1096[6] == 101 && (unsigned char)tb_s1096[7] == 114 && tb_s1096[8] == 0); })) {
        f_emit__emit_together(v_c, v_n, v_depth);
    } else if (({ const char *tb_s1097 = v_k; ((unsigned char)tb_s1097[0] == 116 && (unsigned char)tb_s1097[1] == 101 && (unsigned char)tb_s1097[2] == 109 && (unsigned char)tb_s1097[3] == 112 && tb_s1097[4] == 0); })) {
        { long long tb_v1098 = 1LL; long long *tb_p1098 = &(((tb_o_Comp *)tb_nn(v_c, 1333))->q_label); *tb_p1098 += tb_v1098; }
        const char * v_id = ({ const char *tb_c1099_0 = ""; long long tb_c1099_0n = sizeof("") - 1; char tb_c1099_1d[24]; long long tb_c1099_1n; const char *tb_c1099_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1334))->q_label, tb_c1099_1d, &tb_c1099_1n); char *tb_c1099r = tb_mem(tb_c1099_0n + tb_c1099_1n + 1), *tb_c1099rw = tb_c1099r; memcpy(tb_c1099rw, tb_c1099_0, tb_c1099_0n); tb_c1099rw += tb_c1099_0n; memcpy(tb_c1099rw, tb_c1099_1, tb_c1099_1n); tb_c1099rw += tb_c1099_1n; *tb_c1099rw = 0; (const char *)tb_c1099r; });
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "{\n");
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c1100_0 = "tb_mark tb_k"; long long tb_c1100_0n = sizeof("tb_mark tb_k") - 1; const char *tb_c1100_1 = v_id; long long tb_c1100_1n = (long long)strlen(tb_c1100_1); const char *tb_c1100_2 = " = tb_temp_begin();\n"; long long tb_c1100_2n = sizeof(" = tb_temp_begin();\n") - 1; char *tb_c1100r = tb_mem(tb_c1100_0n + tb_c1100_1n + tb_c1100_2n + 1), *tb_c1100rw = tb_c1100r; memcpy(tb_c1100rw, tb_c1100_0, tb_c1100_0n); tb_c1100rw += tb_c1100_0n; memcpy(tb_c1100rw, tb_c1100_1, tb_c1100_1n); tb_c1100rw += tb_c1100_1n; memcpy(tb_c1100rw, tb_c1100_2, tb_c1100_2n); tb_c1100rw += tb_c1100_2n; *tb_c1100rw = 0; (const char *)tb_c1100r; }));
        f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1339))->q_kids, 0LL, sizeof(tb_o_Node *), 1339)), (v_depth + 1LL));
        f_emit__indent(v_c, (v_depth + 1LL));
        f_emit__put(v_c, ({ const char *tb_c1101_0 = "tb_temp_end(tb_k"; long long tb_c1101_0n = sizeof("tb_temp_end(tb_k") - 1; const char *tb_c1101_1 = v_id; long long tb_c1101_1n = (long long)strlen(tb_c1101_1); const char *tb_c1101_2 = ");\n"; long long tb_c1101_2n = sizeof(");\n") - 1; char *tb_c1101r = tb_mem(tb_c1101_0n + tb_c1101_1n + tb_c1101_2n + 1), *tb_c1101rw = tb_c1101r; memcpy(tb_c1101rw, tb_c1101_0, tb_c1101_0n); tb_c1101rw += tb_c1101_0n; memcpy(tb_c1101rw, tb_c1101_1, tb_c1101_1n); tb_c1101rw += tb_c1101_1n; memcpy(tb_c1101rw, tb_c1101_2, tb_c1101_2n); tb_c1101rw += tb_c1101_2n; *tb_c1101rw = 0; (const char *)tb_c1101r; }));
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "}\n");
    } else if (({ const char *tb_s1102 = v_k; ((unsigned char)tb_s1102[0] == 115 && (unsigned char)tb_s1102[1] == 107 && (unsigned char)tb_s1102[2] == 105 && (unsigned char)tb_s1102[3] == 112 && tb_s1102[4] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "continue;\n");
    } else if (({ const char *tb_s1103 = v_k; ((unsigned char)tb_s1103[0] == 115 && (unsigned char)tb_s1103[1] == 116 && (unsigned char)tb_s1103[2] == 111 && (unsigned char)tb_s1103[3] == 112 && tb_s1103[4] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "break;\n");
    } else if (({ const char *tb_s1104 = v_k; ((unsigned char)tb_s1104[0] == 102 && (unsigned char)tb_s1104[1] == 97 && (unsigned char)tb_s1104[2] == 105 && (unsigned char)tb_s1104[3] == 108 && tb_s1104[4] == 0); })) {
        f_emit__indent(v_c, v_depth);
        f_emit__put(v_c, "{ tb_err = ");
        f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1353))->q_kids, 0LL, sizeof(tb_o_Node *), 1353)));
        f_emit__put(v_c, "; ");
        f_emit__emit_bail(v_c);
        f_emit__put(v_c, " }\n");
    } else if ((({ const char *tb_s1105 = v_k; ((unsigned char)tb_s1105[0] == 114 && (unsigned char)tb_s1105[1] == 101 && (unsigned char)tb_s1105[2] == 116 && (unsigned char)tb_s1105[3] == 117 && (unsigned char)tb_s1105[4] == 114 && (unsigned char)tb_s1105[5] == 110 && tb_s1105[6] == 0); }) && ({ const char *tb_s1106 = ((tb_o_Comp *)tb_nn(v_c, 1357))->q_fmark; !(tb_s1106[0] == 0); }))) {
        if (((((tb_o_Node *)tb_nn(v_n, 1359))->q_kids)->len == 1LL)) {
            f_emit__emit_pre(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1360))->q_kids, 0LL, sizeof(tb_o_Node *), 1360)), v_depth);
            f_emit__indent(v_c, v_depth);
            f_emit__put(v_c, ({ const char *tb_c1107_0 = "{ "; long long tb_c1107_0n = sizeof("{ ") - 1; const char *tb_c1107_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1362))->q_cur_func, 1362))->q_ty); long long tb_c1107_1n = (long long)strlen(tb_c1107_1); const char *tb_c1107_2 = " tb_rv = "; long long tb_c1107_2n = sizeof(" tb_rv = ") - 1; char *tb_c1107r = tb_mem(tb_c1107_0n + tb_c1107_1n + tb_c1107_2n + 1), *tb_c1107rw = tb_c1107r; memcpy(tb_c1107rw, tb_c1107_0, tb_c1107_0n); tb_c1107rw += tb_c1107_0n; memcpy(tb_c1107rw, tb_c1107_1, tb_c1107_1n); tb_c1107rw += tb_c1107_1n; memcpy(tb_c1107rw, tb_c1107_2, tb_c1107_2n); tb_c1107rw += tb_c1107_2n; *tb_c1107rw = 0; (const char *)tb_c1107r; }));
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1363))->q_kids, 0LL, sizeof(tb_o_Node *), 1363)), ((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1363))->q_cur_func, 1363))->q_ty);
            f_emit__put(v_c, ({ const char *tb_c1108_0 = "; if (tb_fa) tb_temp_end("; long long tb_c1108_0n = sizeof("; if (tb_fa) tb_temp_end(") - 1; const char *tb_c1108_1 = ((tb_o_Comp *)tb_nn(v_c, 1364))->q_fmark; long long tb_c1108_1n = (long long)strlen(tb_c1108_1); const char *tb_c1108_2 = "); return tb_rv; }\n"; long long tb_c1108_2n = sizeof("); return tb_rv; }\n") - 1; char *tb_c1108r = tb_mem(tb_c1108_0n + tb_c1108_1n + tb_c1108_2n + 1), *tb_c1108rw = tb_c1108r; memcpy(tb_c1108rw, tb_c1108_0, tb_c1108_0n); tb_c1108rw += tb_c1108_0n; memcpy(tb_c1108rw, tb_c1108_1, tb_c1108_1n); tb_c1108rw += tb_c1108_1n; memcpy(tb_c1108rw, tb_c1108_2, tb_c1108_2n); tb_c1108rw += tb_c1108_2n; *tb_c1108rw = 0; (const char *)tb_c1108r; }));
        } else {
            f_emit__indent(v_c, v_depth);
            f_emit__put(v_c, ({ const char *tb_c1109_0 = "if (tb_fa) tb_temp_end("; long long tb_c1109_0n = sizeof("if (tb_fa) tb_temp_end(") - 1; const char *tb_c1109_1 = ((tb_o_Comp *)tb_nn(v_c, 1367))->q_fmark; long long tb_c1109_1n = (long long)strlen(tb_c1109_1); const char *tb_c1109_2 = ");\n"; long long tb_c1109_2n = sizeof(");\n") - 1; char *tb_c1109r = tb_mem(tb_c1109_0n + tb_c1109_1n + tb_c1109_2n + 1), *tb_c1109rw = tb_c1109r; memcpy(tb_c1109rw, tb_c1109_0, tb_c1109_0n); tb_c1109rw += tb_c1109_0n; memcpy(tb_c1109rw, tb_c1109_1, tb_c1109_1n); tb_c1109rw += tb_c1109_1n; memcpy(tb_c1109rw, tb_c1109_2, tb_c1109_2n); tb_c1109rw += tb_c1109_2n; *tb_c1109rw = 0; (const char *)tb_c1109r; }));
            if (((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Comp *)tb_nn(v_c, 1368))->q_cur_func, 1368))->q_kids, 2LL, sizeof(tb_o_Node *), 1368)), 1368))->q_kids)->len > 0LL)) {
                f_emit__emit_gives_return(v_c, v_depth);
            } else {
                f_emit__indent(v_c, v_depth);
                f_emit__put(v_c, "return;\n");
            }
        }
    } else if (({ const char *tb_s1110 = v_k; ((unsigned char)tb_s1110[0] == 114 && (unsigned char)tb_s1110[1] == 101 && (unsigned char)tb_s1110[2] == 116 && (unsigned char)tb_s1110[3] == 117 && (unsigned char)tb_s1110[4] == 114 && (unsigned char)tb_s1110[5] == 110 && tb_s1110[6] == 0); })) {
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
        if ((({ const char *tb_s1111 = ((tb_o_Node *)tb_nn(v_e, 1388))->q_kind; ((unsigned char)tb_s1111[0] == 116 && (unsigned char)tb_s1111[1] == 114 && (unsigned char)tb_s1111[2] == 121 && tb_s1111[3] == 0); }) || ({ const char *tb_s1112 = ((tb_o_Node *)tb_nn(v_e, 1388))->q_kind; ((unsigned char)tb_s1112[0] == 101 && (unsigned char)tb_s1112[1] == 108 && (unsigned char)tb_s1112[2] == 115 && (unsigned char)tb_s1112[3] == 101 && (unsigned char)tb_s1112[4] == 95 && (unsigned char)tb_s1112[5] == 101 && (unsigned char)tb_s1112[6] == 114 && (unsigned char)tb_s1112[7] == 114 && tb_s1112[8] == 0); }))) {
            f_emit__emit_pre(v_c, v_e, v_depth);
            return;
        }
        f_emit__indent(v_c, v_depth);
        if ((({ const char *tb_s1113 = ((tb_o_Node *)tb_nn(v_e, 1392))->q_kind; ((unsigned char)tb_s1113[0] == 109 && (unsigned char)tb_s1113[1] == 101 && (unsigned char)tb_s1113[2] == 116 && (unsigned char)tb_s1113[3] == 104 && (unsigned char)tb_s1113[4] == 111 && (unsigned char)tb_s1113[5] == 100 && tb_s1113[6] == 0); }) && ({ const char *tb_s1114 = ((tb_o_Node *)tb_nn(v_e, 1392))->q_name; ((unsigned char)tb_s1114[0] == 97 && (unsigned char)tb_s1114[1] == 100 && (unsigned char)tb_s1114[2] == 100 && tb_s1114[3] == 0); }))) {
            long long v_elem = ((tb_o_Type *)tb_nn((*(tb_o_Type * *)tb_at(((tb_o_Comp *)tb_nn(v_c, 1394))->q_types, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1394))->q_kids, 0LL, sizeof(tb_o_Node *), 1394)), 1394))->q_ty, sizeof(tb_o_Type *), 1394)), 1394))->q_elem;
            { long long tb_v1115 = 1LL; long long *tb_p1115 = &(((tb_o_Comp *)tb_nn(v_c, 1395))->q_label); *tb_p1115 += tb_v1115; }
            const char * v_id = ({ const char *tb_c1116_0 = ""; long long tb_c1116_0n = sizeof("") - 1; char tb_c1116_1d[24]; long long tb_c1116_1n; const char *tb_c1116_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1396))->q_label, tb_c1116_1d, &tb_c1116_1n); char *tb_c1116r = tb_mem(tb_c1116_0n + tb_c1116_1n + 1), *tb_c1116rw = tb_c1116r; memcpy(tb_c1116rw, tb_c1116_0, tb_c1116_0n); tb_c1116rw += tb_c1116_0n; memcpy(tb_c1116rw, tb_c1116_1, tb_c1116_1n); tb_c1116rw += tb_c1116_1n; *tb_c1116rw = 0; (const char *)tb_c1116r; });
            tb_o_Node * v_value = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1397))->q_kids, 1LL, sizeof(tb_o_Node *), 1397)), 1397))->q_kids, 0LL, sizeof(tb_o_Node *), 1397));
            if ((((f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1398))->q_kids, 0LL, sizeof(tb_o_Node *), 1398)), 1398))->q_ty) && ({ const char *tb_s1117 = ((tb_o_Node *)tb_nn(v_value, 1398))->q_kind; ((unsigned char)tb_s1117[0] == 99 && (unsigned char)tb_s1117[1] == 97 && (unsigned char)tb_s1117[2] == 108 && (unsigned char)tb_s1117[3] == 108 && tb_s1117[4] == 0); })) && ((tb_o_Node *)tb_nn(v_value, 1398))->q_has_ref) && ({ const char *tb_s1118 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_value, 1398))->q_ref, 1398))->q_kind; ((unsigned char)tb_s1118[0] == 111 && (unsigned char)tb_s1118[1] == 98 && (unsigned char)tb_s1118[2] == 106 && tb_s1118[3] == 0); }))) {
                tb_o_Node * v_o = ((tb_o_Node *)tb_nn(v_value, 1400))->q_ref;
                f_emit__put(v_c, ({ const char *tb_c1119_0 = "tb_colpush_"; long long tb_c1119_0n = sizeof("tb_colpush_") - 1; const char *tb_c1119_1 = ((tb_o_Node *)tb_nn(v_o, 1401))->q_name; long long tb_c1119_1n = (long long)strlen(tb_c1119_1); const char *tb_c1119_2 = "("; long long tb_c1119_2n = sizeof("(") - 1; char *tb_c1119r = tb_mem(tb_c1119_0n + tb_c1119_1n + tb_c1119_2n + 1), *tb_c1119rw = tb_c1119r; memcpy(tb_c1119rw, tb_c1119_0, tb_c1119_0n); tb_c1119rw += tb_c1119_0n; memcpy(tb_c1119rw, tb_c1119_1, tb_c1119_1n); tb_c1119rw += tb_c1119_1n; memcpy(tb_c1119rw, tb_c1119_2, tb_c1119_2n); tb_c1119rw += tb_c1119_2n; *tb_c1119rw = 0; (const char *)tb_c1119r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1402))->q_kids, 0LL, sizeof(tb_o_Node *), 1402)));
                {
                    long long tb_end1120 = ((((tb_o_Node *)tb_nn(v_o, 1403))->q_kids)->len - 1LL);
                    for (long long v_i = 0LL; v_i <= tb_end1120; v_i++) {
                        f_emit__put(v_c, ", ");
                        if (({ const char *tb_s1121 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 1405))->q_kids, v_i, sizeof(tb_o_Node *), 1405)), 1405))->q_kind; ((unsigned char)tb_s1121[0] == 109 && (unsigned char)tb_s1121[1] == 105 && (unsigned char)tb_s1121[2] == 115 && (unsigned char)tb_s1121[3] == 115 && (unsigned char)tb_s1121[4] == 105 && (unsigned char)tb_s1121[5] == 110 && (unsigned char)tb_s1121[6] == 103 && tb_s1121[7] == 0); })) {
                            f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1406))->q_kids, v_i, sizeof(tb_o_Node *), 1406)), 1406))->q_ty);
                        } else {
                            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_value, 1408))->q_kids, v_i, sizeof(tb_o_Node *), 1408)), 1408))->q_kids, 0LL, sizeof(tb_o_Node *), 1408)), ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1408))->q_kids, v_i, sizeof(tb_o_Node *), 1408)), 1408))->q_ty);
                        }
                    }
                }
                f_emit__put(v_c, ({ const char *tb_c1122_0 = ", "; long long tb_c1122_0n = sizeof(", ") - 1; char tb_c1122_1d[24]; long long tb_c1122_1n; const char *tb_c1122_1 = tb_digits(((tb_o_Node *)tb_nn(v_e, 1409))->q_line, tb_c1122_1d, &tb_c1122_1n); const char *tb_c1122_2 = ");\n"; long long tb_c1122_2n = sizeof(");\n") - 1; char *tb_c1122r = tb_mem(tb_c1122_0n + tb_c1122_1n + tb_c1122_2n + 1), *tb_c1122rw = tb_c1122r; memcpy(tb_c1122rw, tb_c1122_0, tb_c1122_0n); tb_c1122rw += tb_c1122_0n; memcpy(tb_c1122rw, tb_c1122_1, tb_c1122_1n); tb_c1122rw += tb_c1122_1n; memcpy(tb_c1122rw, tb_c1122_2, tb_c1122_2n); tb_c1122rw += tb_c1122_2n; *tb_c1122rw = 0; (const char *)tb_c1122r; }));
                return;
            }
            if (f_check__is_cols_list(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1411))->q_kids, 0LL, sizeof(tb_o_Node *), 1411)), 1411))->q_ty)) {
                f_emit__put(v_c, ({ const char *tb_c1123_0 = "{ "; long long tb_c1123_0n = sizeof("{ ") - 1; const char *tb_c1123_1 = f_emit__c_type(v_c, v_elem); long long tb_c1123_1n = (long long)strlen(tb_c1123_1); const char *tb_c1123_2 = " tb_v"; long long tb_c1123_2n = sizeof(" tb_v") - 1; const char *tb_c1123_3 = v_id; long long tb_c1123_3n = (long long)strlen(tb_c1123_3); const char *tb_c1123_4 = " = "; long long tb_c1123_4n = sizeof(" = ") - 1; char *tb_c1123r = tb_mem(tb_c1123_0n + tb_c1123_1n + tb_c1123_2n + tb_c1123_3n + tb_c1123_4n + 1), *tb_c1123rw = tb_c1123r; memcpy(tb_c1123rw, tb_c1123_0, tb_c1123_0n); tb_c1123rw += tb_c1123_0n; memcpy(tb_c1123rw, tb_c1123_1, tb_c1123_1n); tb_c1123rw += tb_c1123_1n; memcpy(tb_c1123rw, tb_c1123_2, tb_c1123_2n); tb_c1123rw += tb_c1123_2n; memcpy(tb_c1123rw, tb_c1123_3, tb_c1123_3n); tb_c1123rw += tb_c1123_3n; memcpy(tb_c1123rw, tb_c1123_4, tb_c1123_4n); tb_c1123rw += tb_c1123_4n; *tb_c1123rw = 0; (const char *)tb_c1123r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1413))->q_kids, 1LL, sizeof(tb_o_Node *), 1413)), 1413))->q_kids, 0LL, sizeof(tb_o_Node *), 1413)));
                f_emit__put(v_c, ({ const char *tb_c1124_0 = "; tb_coladd_"; long long tb_c1124_0n = sizeof("; tb_coladd_") - 1; const char *tb_c1124_1 = f_emit__cols_name(v_c, v_elem); long long tb_c1124_1n = (long long)strlen(tb_c1124_1); const char *tb_c1124_2 = "("; long long tb_c1124_2n = sizeof("(") - 1; char *tb_c1124r = tb_mem(tb_c1124_0n + tb_c1124_1n + tb_c1124_2n + 1), *tb_c1124rw = tb_c1124r; memcpy(tb_c1124rw, tb_c1124_0, tb_c1124_0n); tb_c1124rw += tb_c1124_0n; memcpy(tb_c1124rw, tb_c1124_1, tb_c1124_1n); tb_c1124rw += tb_c1124_1n; memcpy(tb_c1124rw, tb_c1124_2, tb_c1124_2n); tb_c1124rw += tb_c1124_2n; *tb_c1124rw = 0; (const char *)tb_c1124r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1415))->q_kids, 0LL, sizeof(tb_o_Node *), 1415)));
                f_emit__put(v_c, ({ const char *tb_c1125_0 = ", tb_v"; long long tb_c1125_0n = sizeof(", tb_v") - 1; const char *tb_c1125_1 = v_id; long long tb_c1125_1n = (long long)strlen(tb_c1125_1); const char *tb_c1125_2 = ", "; long long tb_c1125_2n = sizeof(", ") - 1; char tb_c1125_3d[24]; long long tb_c1125_3n; const char *tb_c1125_3 = tb_digits(((tb_o_Node *)tb_nn(v_e, 1416))->q_line, tb_c1125_3d, &tb_c1125_3n); const char *tb_c1125_4 = "); }\n"; long long tb_c1125_4n = sizeof("); }\n") - 1; char *tb_c1125r = tb_mem(tb_c1125_0n + tb_c1125_1n + tb_c1125_2n + tb_c1125_3n + tb_c1125_4n + 1), *tb_c1125rw = tb_c1125r; memcpy(tb_c1125rw, tb_c1125_0, tb_c1125_0n); tb_c1125rw += tb_c1125_0n; memcpy(tb_c1125rw, tb_c1125_1, tb_c1125_1n); tb_c1125rw += tb_c1125_1n; memcpy(tb_c1125rw, tb_c1125_2, tb_c1125_2n); tb_c1125rw += tb_c1125_2n; memcpy(tb_c1125rw, tb_c1125_3, tb_c1125_3n); tb_c1125rw += tb_c1125_3n; memcpy(tb_c1125rw, tb_c1125_4, tb_c1125_4n); tb_c1125rw += tb_c1125_4n; *tb_c1125rw = 0; (const char *)tb_c1125r; }));
                return;
            }
            if (f_emit__is_bits(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1418))->q_kids, 0LL, sizeof(tb_o_Node *), 1418)), 1418))->q_ty)) {
                f_emit__put(v_c, ({ const char *tb_c1126_0 = "{ _Bool tb_v"; long long tb_c1126_0n = sizeof("{ _Bool tb_v") - 1; const char *tb_c1126_1 = v_id; long long tb_c1126_1n = (long long)strlen(tb_c1126_1); const char *tb_c1126_2 = " = "; long long tb_c1126_2n = sizeof(" = ") - 1; char *tb_c1126r = tb_mem(tb_c1126_0n + tb_c1126_1n + tb_c1126_2n + 1), *tb_c1126rw = tb_c1126r; memcpy(tb_c1126rw, tb_c1126_0, tb_c1126_0n); tb_c1126rw += tb_c1126_0n; memcpy(tb_c1126rw, tb_c1126_1, tb_c1126_1n); tb_c1126rw += tb_c1126_1n; memcpy(tb_c1126rw, tb_c1126_2, tb_c1126_2n); tb_c1126rw += tb_c1126_2n; *tb_c1126rw = 0; (const char *)tb_c1126r; }));
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1420))->q_kids, 1LL, sizeof(tb_o_Node *), 1420)), 1420))->q_kids, 0LL, sizeof(tb_o_Node *), 1420)));
                f_emit__put(v_c, "; tb_badd(");
                f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1422))->q_kids, 0LL, sizeof(tb_o_Node *), 1422)));
                f_emit__put(v_c, ({ const char *tb_c1127_0 = ", tb_v"; long long tb_c1127_0n = sizeof(", tb_v") - 1; const char *tb_c1127_1 = v_id; long long tb_c1127_1n = (long long)strlen(tb_c1127_1); const char *tb_c1127_2 = "); }\n"; long long tb_c1127_2n = sizeof("); }\n") - 1; char *tb_c1127r = tb_mem(tb_c1127_0n + tb_c1127_1n + tb_c1127_2n + 1), *tb_c1127rw = tb_c1127r; memcpy(tb_c1127rw, tb_c1127_0, tb_c1127_0n); tb_c1127rw += tb_c1127_0n; memcpy(tb_c1127rw, tb_c1127_1, tb_c1127_1n); tb_c1127rw += tb_c1127_1n; memcpy(tb_c1127rw, tb_c1127_2, tb_c1127_2n); tb_c1127rw += tb_c1127_2n; *tb_c1127rw = 0; (const char *)tb_c1127r; }));
                return;
            }
            f_emit__put(v_c, ({ const char *tb_c1128_0 = "{ "; long long tb_c1128_0n = sizeof("{ ") - 1; const char *tb_c1128_1 = f_emit__c_type(v_c, v_elem); long long tb_c1128_1n = (long long)strlen(tb_c1128_1); const char *tb_c1128_2 = " tb_v"; long long tb_c1128_2n = sizeof(" tb_v") - 1; const char *tb_c1128_3 = v_id; long long tb_c1128_3n = (long long)strlen(tb_c1128_3); const char *tb_c1128_4 = " = "; long long tb_c1128_4n = sizeof(" = ") - 1; char *tb_c1128r = tb_mem(tb_c1128_0n + tb_c1128_1n + tb_c1128_2n + tb_c1128_3n + tb_c1128_4n + 1), *tb_c1128rw = tb_c1128r; memcpy(tb_c1128rw, tb_c1128_0, tb_c1128_0n); tb_c1128rw += tb_c1128_0n; memcpy(tb_c1128rw, tb_c1128_1, tb_c1128_1n); tb_c1128rw += tb_c1128_1n; memcpy(tb_c1128rw, tb_c1128_2, tb_c1128_2n); tb_c1128rw += tb_c1128_2n; memcpy(tb_c1128rw, tb_c1128_3, tb_c1128_3n); tb_c1128rw += tb_c1128_3n; memcpy(tb_c1128rw, tb_c1128_4, tb_c1128_4n); tb_c1128rw += tb_c1128_4n; *tb_c1128rw = 0; (const char *)tb_c1128r; }));
            f_emit__emit_as(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1426))->q_kids, 1LL, sizeof(tb_o_Node *), 1426)), 1426))->q_kids, 0LL, sizeof(tb_o_Node *), 1426)), v_elem);
            f_emit__put(v_c, ({ const char *tb_c1129_0 = "; *("; long long tb_c1129_0n = sizeof("; *(") - 1; const char *tb_c1129_1 = f_emit__c_type(v_c, v_elem); long long tb_c1129_1n = (long long)strlen(tb_c1129_1); const char *tb_c1129_2 = " *)tb_grow("; long long tb_c1129_2n = sizeof(" *)tb_grow(") - 1; char *tb_c1129r = tb_mem(tb_c1129_0n + tb_c1129_1n + tb_c1129_2n + 1), *tb_c1129rw = tb_c1129r; memcpy(tb_c1129rw, tb_c1129_0, tb_c1129_0n); tb_c1129rw += tb_c1129_0n; memcpy(tb_c1129rw, tb_c1129_1, tb_c1129_1n); tb_c1129rw += tb_c1129_1n; memcpy(tb_c1129rw, tb_c1129_2, tb_c1129_2n); tb_c1129rw += tb_c1129_2n; *tb_c1129rw = 0; (const char *)tb_c1129r; }));
            f_emit__emit_expr(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_e, 1428))->q_kids, 0LL, sizeof(tb_o_Node *), 1428)));
            f_emit__put(v_c, ({ const char *tb_c1130_0 = ") = tb_v"; long long tb_c1130_0n = sizeof(") = tb_v") - 1; const char *tb_c1130_1 = v_id; long long tb_c1130_1n = (long long)strlen(tb_c1130_1); const char *tb_c1130_2 = "; }\n"; long long tb_c1130_2n = sizeof("; }\n") - 1; char *tb_c1130r = tb_mem(tb_c1130_0n + tb_c1130_1n + tb_c1130_2n + 1), *tb_c1130rw = tb_c1130r; memcpy(tb_c1130rw, tb_c1130_0, tb_c1130_0n); tb_c1130rw += tb_c1130_0n; memcpy(tb_c1130rw, tb_c1130_1, tb_c1130_1n); tb_c1130rw += tb_c1130_1n; memcpy(tb_c1130rw, tb_c1130_2, tb_c1130_2n); tb_c1130rw += tb_c1130_2n; *tb_c1130rw = 0; (const char *)tb_c1130r; }));
        } else {
            f_emit__emit_expr(v_c, v_e);
            f_emit__put(v_c, ";\n");
        }
    }
}

static void f_emit__emit_block(tb_o_Comp * v_c, tb_o_Node * v_block, long long v_depth) {
    {
        tb_list * tb_l1131 = ((tb_o_Node *)tb_nn(v_block, 1435))->q_kids;
        for (long long tb_i1131 = 0; tb_i1131 < tb_l1131->len; tb_i1131++) {
            tb_o_Node * const v_s = ((tb_o_Node * *)tb_l1131->data)[tb_i1131];
            f_emit__emit_statement(v_c, v_s, v_depth);
        }
    }
}

static void f_emit__emit_signature(tb_o_Comp * v_c, tb_o_Node * v_f) {
    tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1441))->q_kids, 0LL, sizeof(tb_o_Node *), 1441));
    f_emit__put(v_c, ({ const char *tb_c1132_0 = "static "; long long tb_c1132_0n = sizeof("static ") - 1; const char *tb_c1132_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1442))->q_ty); long long tb_c1132_1n = (long long)strlen(tb_c1132_1); const char *tb_c1132_2 = " "; long long tb_c1132_2n = sizeof(" ") - 1; const char *tb_c1132_3 = f_emit__fname(v_f); long long tb_c1132_3n = (long long)strlen(tb_c1132_3); const char *tb_c1132_4 = "("; long long tb_c1132_4n = sizeof("(") - 1; char *tb_c1132r = tb_mem(tb_c1132_0n + tb_c1132_1n + tb_c1132_2n + tb_c1132_3n + tb_c1132_4n + 1), *tb_c1132rw = tb_c1132r; memcpy(tb_c1132rw, tb_c1132_0, tb_c1132_0n); tb_c1132rw += tb_c1132_0n; memcpy(tb_c1132rw, tb_c1132_1, tb_c1132_1n); tb_c1132rw += tb_c1132_1n; memcpy(tb_c1132rw, tb_c1132_2, tb_c1132_2n); tb_c1132rw += tb_c1132_2n; memcpy(tb_c1132rw, tb_c1132_3, tb_c1132_3n); tb_c1132rw += tb_c1132_3n; memcpy(tb_c1132rw, tb_c1132_4, tb_c1132_4n); tb_c1132rw += tb_c1132_4n; *tb_c1132rw = 0; (const char *)tb_c1132r; }));
    if (((((tb_o_Node *)tb_nn(v_params, 1443))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end1133 = ((((tb_o_Node *)tb_nn(v_params, 1445))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1133; v_i++) {
            tb_o_Node * v_p = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1446))->q_kids, v_i, sizeof(tb_o_Node *), 1446));
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            if (((tb_o_Node *)tb_nn(v_p, 1449))->q_is_link) {
                f_emit__put(v_c, ({ const char *tb_c1134_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1450))->q_ty); long long tb_c1134_0n = (long long)strlen(tb_c1134_0); const char *tb_c1134_1 = " *v_"; long long tb_c1134_1n = sizeof(" *v_") - 1; const char *tb_c1134_2 = ((tb_o_Node *)tb_nn(v_p, 1450))->q_name; long long tb_c1134_2n = (long long)strlen(tb_c1134_2); char *tb_c1134r = tb_mem(tb_c1134_0n + tb_c1134_1n + tb_c1134_2n + 1), *tb_c1134rw = tb_c1134r; memcpy(tb_c1134rw, tb_c1134_0, tb_c1134_0n); tb_c1134rw += tb_c1134_0n; memcpy(tb_c1134rw, tb_c1134_1, tb_c1134_1n); tb_c1134rw += tb_c1134_1n; memcpy(tb_c1134rw, tb_c1134_2, tb_c1134_2n); tb_c1134rw += tb_c1134_2n; *tb_c1134rw = 0; (const char *)tb_c1134r; }));
            } else {
                f_emit__put(v_c, ({ const char *tb_c1135_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_p, 1452))->q_ty); long long tb_c1135_0n = (long long)strlen(tb_c1135_0); const char *tb_c1135_1 = " v_"; long long tb_c1135_1n = sizeof(" v_") - 1; const char *tb_c1135_2 = ((tb_o_Node *)tb_nn(v_p, 1452))->q_name; long long tb_c1135_2n = (long long)strlen(tb_c1135_2); char *tb_c1135r = tb_mem(tb_c1135_0n + tb_c1135_1n + tb_c1135_2n + 1), *tb_c1135rw = tb_c1135r; memcpy(tb_c1135rw, tb_c1135_0, tb_c1135_0n); tb_c1135rw += tb_c1135_0n; memcpy(tb_c1135rw, tb_c1135_1, tb_c1135_1n); tb_c1135rw += tb_c1135_1n; memcpy(tb_c1135rw, tb_c1135_2, tb_c1135_2n); tb_c1135rw += tb_c1135_2n; *tb_c1135rw = 0; (const char *)tb_c1135r; }));
            }
        }
    }
    f_emit__put(v_c, ")");
}

static _Bool f_emit__bld_has(tb_list * v_names, const char * v_name) {
    {
        tb_list * tb_l1136 = v_names;
        for (long long tb_i1136 = 0; tb_i1136 < tb_l1136->len; tb_i1136++) {
            const char * const v_x = ((const char * *)tb_l1136->data)[tb_i1136];
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
    if ((({ const char *tb_s1137 = ((tb_o_Node *)tb_nn(v_n, 1469))->q_kind; ((unsigned char)tb_s1137[0] == 110 && (unsigned char)tb_s1137[1] == 97 && (unsigned char)tb_s1137[2] == 109 && (unsigned char)tb_s1137[3] == 101 && tb_s1137[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_n, 1469))->q_name, v_name) == 0))) {
        return 1;
    }
    {
        tb_list * tb_l1138 = ((tb_o_Node *)tb_nn(v_n, 1471))->q_kids;
        for (long long tb_i1138 = 0; tb_i1138 < tb_l1138->len; tb_i1138++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l1138->data)[tb_i1138];
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
    if ((({ const char *tb_s1139 = ((tb_o_Node *)tb_nn(v_n, 1478))->q_kind; ((unsigned char)tb_s1139[0] == 110 && (unsigned char)tb_s1139[1] == 97 && (unsigned char)tb_s1139[2] == 109 && (unsigned char)tb_s1139[3] == 101 && tb_s1139[4] == 0); }) || ({ const char *tb_s1140 = ((tb_o_Node *)tb_nn(v_n, 1478))->q_kind; ((unsigned char)tb_s1140[0] == 97 && (unsigned char)tb_s1140[1] == 115 && (unsigned char)tb_s1140[2] == 115 && (unsigned char)tb_s1140[3] == 105 && (unsigned char)tb_s1140[4] == 103 && (unsigned char)tb_s1140[5] == 110 && tb_s1140[6] == 0); }))) {
        { const char * tb_v1141 = ((tb_o_Node *)tb_nn(v_n, 1479))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1141; }
    }
    {
        tb_list * tb_l1142 = ((tb_o_Node *)tb_nn(v_n, 1480))->q_kids;
        for (long long tb_i1142 = 0; tb_i1142 < tb_l1142->len; tb_i1142++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l1142->data)[tb_i1142];
            f_emit__bld_bad_all(v_k, v_bad);
        }
    }
}

static _Bool f_emit__is_text_plus(tb_o_Comp * v_c, tb_o_Node * v_n) {
    return ((({ const char *tb_s1143 = ((tb_o_Node *)tb_nn(v_n, 1484))->q_kind; ((unsigned char)tb_s1143[0] == 98 && (unsigned char)tb_s1143[1] == 105 && (unsigned char)tb_s1143[2] == 110 && (unsigned char)tb_s1143[3] == 111 && (unsigned char)tb_s1143[4] == 112 && tb_s1143[5] == 0); }) && ({ const char *tb_s1144 = ((tb_o_Node *)tb_nn(v_n, 1484))->q_value; ((unsigned char)tb_s1144[0] == 43 && tb_s1144[1] == 0); })) && ({ const char *tb_s1145 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 1484))->q_ty); ((unsigned char)tb_s1145[0] == 116 && (unsigned char)tb_s1145[1] == 101 && (unsigned char)tb_s1145[2] == 120 && (unsigned char)tb_s1145[3] == 116 && tb_s1145[4] == 0); }));
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
    return ((({ const char *tb_s1146 = ((tb_o_Node *)tb_nn(v_r, 1495))->q_kind; ((unsigned char)tb_s1146[0] == 110 && (unsigned char)tb_s1146[1] == 97 && (unsigned char)tb_s1146[2] == 109 && (unsigned char)tb_s1146[3] == 101 && tb_s1146[4] == 0); }) && (strcmp(((tb_o_Node *)tb_nn(v_r, 1495))->q_name, v_name) == 0)) && (!((tb_o_Node *)tb_nn(v_r, 1495))->q_is_link));
    tb_crash(1487, "функция app_form дошла до конца без return");
    return 0;
}

static tb_list * f_emit__app_parts(tb_o_Comp * v_c, tb_o_Node * v_v) {
    tb_list * v_parts = tb_list_new(sizeof(tb_o_Node *));
    tb_list * v_back = tb_list_new(sizeof(tb_o_Node *));
    tb_o_Node * v_r = v_v;
    while (f_emit__is_text_plus(v_c, v_r)) {
        { tb_o_Node * tb_v1147 = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1502))->q_kids, 1LL, sizeof(tb_o_Node *), 1502)); *(tb_o_Node * *)tb_grow(v_back) = tb_v1147; }
        v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1503))->q_kids, 0LL, sizeof(tb_o_Node *), 1503));
    }
    long long v_i = ((v_back)->len - 1LL);
    while ((v_i >= 0LL)) {
        { tb_o_Node * tb_v1148 = (*(tb_o_Node * *)tb_at(v_back, v_i, sizeof(tb_o_Node *), 1506)); *(tb_o_Node * *)tb_grow(v_parts) = tb_v1148; }
        { long long tb_v1149 = 1LL; long long *tb_p1149 = &(v_i); *tb_p1149 -= tb_v1149; }
    }
    return v_parts;
}

static void f_emit__bld_scan(tb_o_Comp * v_c, tb_o_Node * v_n, _Bool v_in_par, tb_list * v_cand, tb_list * v_bad) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1510))->q_kind;
    _Bool v_par = (((v_in_par || (strcmp(v_k, "background") == 0)) || ({ const char *tb_s1150 = v_k; ((unsigned char)tb_s1150[0] == 116 && (unsigned char)tb_s1150[1] == 111 && (unsigned char)tb_s1150[2] == 103 && (unsigned char)tb_s1150[3] == 101 && (unsigned char)tb_s1150[4] == 116 && (unsigned char)tb_s1150[5] == 104 && (unsigned char)tb_s1150[6] == 101 && (unsigned char)tb_s1150[7] == 114 && tb_s1150[8] == 0); })) || ((({ const char *tb_s1151 = v_k; ((unsigned char)tb_s1151[0] == 101 && (unsigned char)tb_s1151[1] == 97 && (unsigned char)tb_s1151[2] == 99 && (unsigned char)tb_s1151[3] == 104 && tb_s1151[4] == 0); }) || ({ const char *tb_s1152 = v_k; ((unsigned char)tb_s1152[0] == 99 && (unsigned char)tb_s1152[1] == 111 && (unsigned char)tb_s1152[2] == 117 && (unsigned char)tb_s1152[3] == 110 && (unsigned char)tb_s1152[4] == 116 && tb_s1152[5] == 0); })) && (((tb_o_Node *)tb_nn(v_n, 1511))->q_is_par || ((tb_o_Node *)tb_nn(v_n, 1511))->q_by_vec)));
    if (v_par) {
        f_emit__bld_bad_all(v_n, v_bad);
        return;
    }
    if ((({ const char *tb_s1153 = v_k; ((unsigned char)tb_s1153[0] == 108 && (unsigned char)tb_s1153[1] == 105 && (unsigned char)tb_s1153[2] == 110 && (unsigned char)tb_s1153[3] == 107 && (unsigned char)tb_s1153[4] == 100 && (unsigned char)tb_s1153[5] == 101 && (unsigned char)tb_s1153[6] == 99 && (unsigned char)tb_s1153[7] == 108 && tb_s1153[8] == 0); }) || ({ const char *tb_s1154 = v_k; ((unsigned char)tb_s1154[0] == 109 && (unsigned char)tb_s1154[1] == 117 && (unsigned char)tb_s1154[2] == 108 && (unsigned char)tb_s1154[3] == 116 && (unsigned char)tb_s1154[4] == 105 && tb_s1154[5] == 0); }))) {
        f_emit__bld_bad_all(v_n, v_bad);
    }
    if (((({ const char *tb_s1155 = v_k; ((unsigned char)tb_s1155[0] == 101 && (unsigned char)tb_s1155[1] == 97 && (unsigned char)tb_s1155[2] == 99 && (unsigned char)tb_s1155[3] == 104 && tb_s1155[4] == 0); }) || ({ const char *tb_s1156 = v_k; ((unsigned char)tb_s1156[0] == 99 && (unsigned char)tb_s1156[1] == 111 && (unsigned char)tb_s1156[2] == 117 && (unsigned char)tb_s1156[3] == 110 && (unsigned char)tb_s1156[4] == 116 && tb_s1156[5] == 0); })) || ({ const char *tb_s1157 = v_k; ((unsigned char)tb_s1157[0] == 101 && (unsigned char)tb_s1157[1] == 108 && (unsigned char)tb_s1157[2] == 115 && (unsigned char)tb_s1157[3] == 101 && (unsigned char)tb_s1157[4] == 95 && (unsigned char)tb_s1157[5] == 101 && (unsigned char)tb_s1157[6] == 114 && (unsigned char)tb_s1157[7] == 114 && tb_s1157[8] == 0); }))) {
        { const char * tb_v1158 = ((tb_o_Node *)tb_nn(v_n, 1518))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1158; }
    }
    if (((({ const char *tb_s1159 = v_k; ((unsigned char)tb_s1159[0] == 99 && (unsigned char)tb_s1159[1] == 97 && (unsigned char)tb_s1159[2] == 108 && (unsigned char)tb_s1159[3] == 108 && tb_s1159[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1519))->q_has_ref) && ({ const char *tb_s1160 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1519))->q_ref, 1519))->q_kind; ((unsigned char)tb_s1160[0] == 102 && (unsigned char)tb_s1160[1] == 117 && (unsigned char)tb_s1160[2] == 110 && (unsigned char)tb_s1160[3] == 99 && tb_s1160[4] == 0); }))) {
        tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1520))->q_ref, 1520))->q_kids, 0LL, sizeof(tb_o_Node *), 1520));
        {
            long long tb_end1161 = ((((tb_o_Node *)tb_nn(v_n, 1521))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end1161; v_i++) {
                if (((v_i < (((tb_o_Node *)tb_nn(v_params, 1522))->q_kids)->len) && ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_params, 1522))->q_kids, v_i, sizeof(tb_o_Node *), 1522)), 1522))->q_is_link)) {
                    f_emit__bld_bad_all((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1523))->q_kids, v_i, sizeof(tb_o_Node *), 1523)), v_bad);
                }
            }
        }
    }
    if (({ const char *tb_s1162 = v_k; ((unsigned char)tb_s1162[0] == 97 && (unsigned char)tb_s1162[1] == 115 && (unsigned char)tb_s1162[2] == 115 && (unsigned char)tb_s1162[3] == 105 && (unsigned char)tb_s1162[4] == 103 && (unsigned char)tb_s1162[5] == 110 && tb_s1162[6] == 0); })) {
        if (((tb_o_Node *)tb_nn(v_n, 1525))->q_is_link) {
            { const char * tb_v1163 = ((tb_o_Node *)tb_nn(v_n, 1526))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1163; }
        } else if ((((({ const char *tb_s1164 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 1527))->q_ty); ((unsigned char)tb_s1164[0] == 116 && (unsigned char)tb_s1164[1] == 101 && (unsigned char)tb_s1164[2] == 120 && (unsigned char)tb_s1164[3] == 116 && tb_s1164[4] == 0); }) && ((((tb_o_Node *)tb_nn(v_n, 1527))->q_kids)->len == 1LL)) && (!((tb_o_Node *)tb_nn(v_n, 1527))->q_creates)) && f_emit__app_form(v_c, ((tb_o_Node *)tb_nn(v_n, 1527))->q_name, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1527))->q_kids, 0LL, sizeof(tb_o_Node *), 1527))))) {
            { const char * tb_v1165 = ((tb_o_Node *)tb_nn(v_n, 1528))->q_name; *(const char * *)tb_grow(v_cand) = tb_v1165; }
        }
    }
    if ((((({ const char *tb_s1166 = v_k; ((unsigned char)tb_s1166[0] == 115 && (unsigned char)tb_s1166[1] == 101 && (unsigned char)tb_s1166[2] == 116 && tb_s1166[3] == 0); }) && ({ const char *tb_s1167 = ((tb_o_Node *)tb_nn(v_n, 1529))->q_value; ((unsigned char)tb_s1167[0] == 43 && (unsigned char)tb_s1167[1] == 61 && tb_s1167[2] == 0); })) && ({ const char *tb_s1168 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1529))->q_kids, 0LL, sizeof(tb_o_Node *), 1529)), 1529))->q_kind; ((unsigned char)tb_s1168[0] == 110 && (unsigned char)tb_s1168[1] == 97 && (unsigned char)tb_s1168[2] == 109 && (unsigned char)tb_s1168[3] == 101 && tb_s1168[4] == 0); })) && ({ const char *tb_s1169 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1529))->q_kids, 0LL, sizeof(tb_o_Node *), 1529)), 1529))->q_ty); ((unsigned char)tb_s1169[0] == 116 && (unsigned char)tb_s1169[1] == 101 && (unsigned char)tb_s1169[2] == 120 && (unsigned char)tb_s1169[3] == 116 && tb_s1169[4] == 0); }))) {
        if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1530))->q_kids, 0LL, sizeof(tb_o_Node *), 1530)), 1530))->q_is_link) {
            { const char * tb_v1170 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1531))->q_kids, 0LL, sizeof(tb_o_Node *), 1531)), 1531))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1170; }
        } else {
            { const char * tb_v1171 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1533))->q_kids, 0LL, sizeof(tb_o_Node *), 1533)), 1533))->q_name; *(const char * *)tb_grow(v_cand) = tb_v1171; }
        }
    }
    {
        tb_list * tb_l1172 = ((tb_o_Node *)tb_nn(v_n, 1534))->q_kids;
        for (long long tb_i1172 = 0; tb_i1172 < tb_l1172->len; tb_i1172++) {
            tb_o_Node * const v_kid = ((tb_o_Node * *)tb_l1172->data)[tb_i1172];
            f_emit__bld_scan(v_c, v_kid, 0, v_cand, v_bad);
        }
    }
}

static void f_emit__find_builders(tb_o_Comp * v_c, tb_o_Node * v_f) {
    tb_list * v_fresh = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1173 = v_fresh; tb_list * *tb_p1173 = &(((tb_o_Comp *)tb_nn(v_c, 1539))->q_builders); *tb_p1173 = tb_v1173; }
    tb_list * v_cand = tb_list_new(sizeof(const char *));
    tb_list * v_bad = tb_list_new(sizeof(const char *));
    {
        tb_list * tb_l1174 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1542))->q_kids, 0LL, sizeof(tb_o_Node *), 1542)), 1542))->q_kids;
        for (long long tb_i1174 = 0; tb_i1174 < tb_l1174->len; tb_i1174++) {
            tb_o_Node * const v_p = ((tb_o_Node * *)tb_l1174->data)[tb_i1174];
            if (((tb_o_Node *)tb_nn(v_p, 1543))->q_is_link) {
                { const char * tb_v1175 = ((tb_o_Node *)tb_nn(v_p, 1544))->q_name; *(const char * *)tb_grow(v_bad) = tb_v1175; }
            }
        }
    }
    f_emit__bld_scan(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1545))->q_kids, 1LL, sizeof(tb_o_Node *), 1545)), 0, v_cand, v_bad);
    {
        tb_list * tb_l1176 = v_cand;
        for (long long tb_i1176 = 0; tb_i1176 < tb_l1176->len; tb_i1176++) {
            const char * const v_name = ((const char * *)tb_l1176->data)[tb_i1176];
            if (((!f_emit__bld_has(v_bad, v_name)) && (!f_emit__bld_has(((tb_o_Comp *)tb_nn(v_c, 1547))->q_builders, v_name)))) {
                { const char * tb_v1177 = v_name; *(const char * *)tb_grow(((tb_o_Comp *)tb_nn(v_c, 1548))->q_builders) = tb_v1177; }
            }
        }
    }
}

static _Bool f_emit__is_bld(tb_o_Comp * v_c, tb_o_Node * v_n) {
    return ((((({ const char *tb_s1178 = ((tb_o_Node *)tb_nn(v_n, 1552))->q_kind; ((unsigned char)tb_s1178[0] == 110 && (unsigned char)tb_s1178[1] == 97 && (unsigned char)tb_s1178[2] == 109 && (unsigned char)tb_s1178[3] == 101 && tb_s1178[4] == 0); }) && (!((tb_o_Node *)tb_nn(v_n, 1552))->q_is_link)) && ((((tb_o_Comp *)tb_nn(v_c, 1552))->q_builders)->len > 0LL)) && ({ const char *tb_s1179 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_n, 1552))->q_ty); ((unsigned char)tb_s1179[0] == 116 && (unsigned char)tb_s1179[1] == 101 && (unsigned char)tb_s1179[2] == 120 && (unsigned char)tb_s1179[3] == 116 && tb_s1179[4] == 0); })) && f_emit__bld_has(((tb_o_Comp *)tb_nn(v_c, 1552))->q_builders, ((tb_o_Node *)tb_nn(v_n, 1552))->q_name));
    tb_crash(1551, "функция is_bld дошла до конца без return");
    return 0;
}

static void f_emit__emit_q(tb_o_Comp * v_c, tb_o_Node * v_n) {
    if (f_emit__is_bld(v_c, v_n)) {
        f_emit__put(v_c, ({ const char *tb_c1180_0 = "v_"; long long tb_c1180_0n = sizeof("v_") - 1; const char *tb_c1180_1 = ((tb_o_Node *)tb_nn(v_n, 1557))->q_name; long long tb_c1180_1n = (long long)strlen(tb_c1180_1); char *tb_c1180r = tb_mem(tb_c1180_0n + tb_c1180_1n + 1), *tb_c1180rw = tb_c1180r; memcpy(tb_c1180rw, tb_c1180_0, tb_c1180_0n); tb_c1180rw += tb_c1180_0n; memcpy(tb_c1180rw, tb_c1180_1, tb_c1180_1n); tb_c1180rw += tb_c1180_1n; *tb_c1180rw = 0; (const char *)tb_c1180r; }));
    } else {
        f_emit__emit_expr(v_c, v_n);
    }
}

static void f_emit__emit_app(tb_o_Comp * v_c, const char * v_name, tb_o_Node * v_part) {
    const char * v_vars = ({ const char *tb_c1181_0 = "&v_"; long long tb_c1181_0n = sizeof("&v_") - 1; const char *tb_c1181_1 = v_name; long long tb_c1181_1n = (long long)strlen(tb_c1181_1); const char *tb_c1181_2 = ", &tb_rl_"; long long tb_c1181_2n = sizeof(", &tb_rl_") - 1; const char *tb_c1181_3 = v_name; long long tb_c1181_3n = (long long)strlen(tb_c1181_3); const char *tb_c1181_4 = ", &tb_rc_"; long long tb_c1181_4n = sizeof(", &tb_rc_") - 1; const char *tb_c1181_5 = v_name; long long tb_c1181_5n = (long long)strlen(tb_c1181_5); const char *tb_c1181_6 = ", "; long long tb_c1181_6n = sizeof(", ") - 1; char *tb_c1181r = tb_mem(tb_c1181_0n + tb_c1181_1n + tb_c1181_2n + tb_c1181_3n + tb_c1181_4n + tb_c1181_5n + tb_c1181_6n + 1), *tb_c1181rw = tb_c1181r; memcpy(tb_c1181rw, tb_c1181_0, tb_c1181_0n); tb_c1181rw += tb_c1181_0n; memcpy(tb_c1181rw, tb_c1181_1, tb_c1181_1n); tb_c1181rw += tb_c1181_1n; memcpy(tb_c1181rw, tb_c1181_2, tb_c1181_2n); tb_c1181rw += tb_c1181_2n; memcpy(tb_c1181rw, tb_c1181_3, tb_c1181_3n); tb_c1181rw += tb_c1181_3n; memcpy(tb_c1181rw, tb_c1181_4, tb_c1181_4n); tb_c1181rw += tb_c1181_4n; memcpy(tb_c1181rw, tb_c1181_5, tb_c1181_5n); tb_c1181rw += tb_c1181_5n; memcpy(tb_c1181rw, tb_c1181_6, tb_c1181_6n); tb_c1181rw += tb_c1181_6n; *tb_c1181rw = 0; (const char *)tb_c1181r; });
    if (({ const char *tb_s1182 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_part, 1563))->q_ty); ((unsigned char)tb_s1182[0] == 105 && (unsigned char)tb_s1182[1] == 110 && (unsigned char)tb_s1182[2] == 116 && tb_s1182[3] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c1183_0 = "tb_app_i("; long long tb_c1183_0n = sizeof("tb_app_i(") - 1; const char *tb_c1183_1 = v_vars; long long tb_c1183_1n = (long long)strlen(tb_c1183_1); char *tb_c1183r = tb_mem(tb_c1183_0n + tb_c1183_1n + 1), *tb_c1183rw = tb_c1183r; memcpy(tb_c1183rw, tb_c1183_0, tb_c1183_0n); tb_c1183rw += tb_c1183_0n; memcpy(tb_c1183rw, tb_c1183_1, tb_c1183_1n); tb_c1183rw += tb_c1183_1n; *tb_c1183rw = 0; (const char *)tb_c1183r; }));
        f_emit__emit_expr(v_c, v_part);
        f_emit__put(v_c, ");");
    } else if (({ const char *tb_s1184 = ((tb_o_Node *)tb_nn(v_part, 1568))->q_kind; ((unsigned char)tb_s1184[0] == 115 && (unsigned char)tb_s1184[1] == 116 && (unsigned char)tb_s1184[2] == 114 && (unsigned char)tb_s1184[3] == 105 && (unsigned char)tb_s1184[4] == 110 && (unsigned char)tb_s1184[5] == 103 && tb_s1184[6] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c1185_0 = "tb_app_n("; long long tb_c1185_0n = sizeof("tb_app_n(") - 1; const char *tb_c1185_1 = v_vars; long long tb_c1185_1n = (long long)strlen(tb_c1185_1); const char *tb_c1185_2 = ((tb_o_Node *)tb_nn(v_part, 1570))->q_value; long long tb_c1185_2n = (long long)strlen(tb_c1185_2); const char *tb_c1185_3 = ", sizeof("; long long tb_c1185_3n = sizeof(", sizeof(") - 1; const char *tb_c1185_4 = ((tb_o_Node *)tb_nn(v_part, 1570))->q_value; long long tb_c1185_4n = (long long)strlen(tb_c1185_4); const char *tb_c1185_5 = ") - 1);"; long long tb_c1185_5n = sizeof(") - 1);") - 1; char *tb_c1185r = tb_mem(tb_c1185_0n + tb_c1185_1n + tb_c1185_2n + tb_c1185_3n + tb_c1185_4n + tb_c1185_5n + 1), *tb_c1185rw = tb_c1185r; memcpy(tb_c1185rw, tb_c1185_0, tb_c1185_0n); tb_c1185rw += tb_c1185_0n; memcpy(tb_c1185rw, tb_c1185_1, tb_c1185_1n); tb_c1185rw += tb_c1185_1n; memcpy(tb_c1185rw, tb_c1185_2, tb_c1185_2n); tb_c1185rw += tb_c1185_2n; memcpy(tb_c1185rw, tb_c1185_3, tb_c1185_3n); tb_c1185rw += tb_c1185_3n; memcpy(tb_c1185rw, tb_c1185_4, tb_c1185_4n); tb_c1185rw += tb_c1185_4n; memcpy(tb_c1185rw, tb_c1185_5, tb_c1185_5n); tb_c1185rw += tb_c1185_5n; *tb_c1185rw = 0; (const char *)tb_c1185r; }));
    } else {
        f_emit__put(v_c, ({ const char *tb_c1186_0 = "tb_app("; long long tb_c1186_0n = sizeof("tb_app(") - 1; const char *tb_c1186_1 = v_vars; long long tb_c1186_1n = (long long)strlen(tb_c1186_1); char *tb_c1186r = tb_mem(tb_c1186_0n + tb_c1186_1n + 1), *tb_c1186rw = tb_c1186r; memcpy(tb_c1186rw, tb_c1186_0, tb_c1186_0n); tb_c1186rw += tb_c1186_0n; memcpy(tb_c1186rw, tb_c1186_1, tb_c1186_1n); tb_c1186rw += tb_c1186_1n; *tb_c1186rw = 0; (const char *)tb_c1186r; }));
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
    if (((((({ const char *tb_s1187 = v_k; ((unsigned char)tb_s1187[0] == 97 && (unsigned char)tb_s1187[1] == 115 && (unsigned char)tb_s1187[2] == 115 && (unsigned char)tb_s1187[3] == 105 && (unsigned char)tb_s1187[4] == 103 && (unsigned char)tb_s1187[5] == 110 && tb_s1187[6] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1587))->q_creates) || ({ const char *tb_s1188 = v_k; ((unsigned char)tb_s1188[0] == 101 && (unsigned char)tb_s1188[1] == 97 && (unsigned char)tb_s1188[2] == 99 && (unsigned char)tb_s1188[3] == 104 && tb_s1188[4] == 0); })) || ({ const char *tb_s1189 = v_k; ((unsigned char)tb_s1189[0] == 99 && (unsigned char)tb_s1189[1] == 111 && (unsigned char)tb_s1189[2] == 117 && (unsigned char)tb_s1189[3] == 110 && (unsigned char)tb_s1189[4] == 116 && tb_s1189[5] == 0); })) || ({ const char *tb_s1190 = v_k; ((unsigned char)tb_s1190[0] == 101 && (unsigned char)tb_s1190[1] == 108 && (unsigned char)tb_s1190[2] == 115 && (unsigned char)tb_s1190[3] == 101 && (unsigned char)tb_s1190[4] == 95 && (unsigned char)tb_s1190[5] == 101 && (unsigned char)tb_s1190[6] == 114 && (unsigned char)tb_s1190[7] == 114 && tb_s1190[8] == 0); }))) {
        { const char * tb_v1191 = ((tb_o_Node *)tb_nn(v_n, 1588))->q_name; *(const char * *)tb_grow(v_inner) = tb_v1191; }
    }
    if (({ const char *tb_s1192 = v_k; ((unsigned char)tb_s1192[0] == 109 && (unsigned char)tb_s1192[1] == 117 && (unsigned char)tb_s1192[2] == 108 && (unsigned char)tb_s1192[3] == 116 && (unsigned char)tb_s1192[4] == 105 && tb_s1192[5] == 0); })) {
        {
            long long tb_end1193 = ((((tb_o_Node *)tb_nn(v_n, 1590))->q_kids)->len - 2LL);
            for (long long v_i = 0LL; v_i <= tb_end1193; v_i++) {
                if (((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1591))->q_kids, v_i, sizeof(tb_o_Node *), 1591)), 1591))->q_creates) {
                    { const char * tb_v1194 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1592))->q_kids, v_i, sizeof(tb_o_Node *), 1592)), 1592))->q_name; *(const char * *)tb_grow(v_inner) = tb_v1194; }
                }
            }
        }
    }
    {
        tb_list * tb_l1195 = ((tb_o_Node *)tb_nn(v_n, 1593))->q_kids;
        for (long long tb_i1195 = 0; tb_i1195 < tb_l1195->len; tb_i1195++) {
            tb_o_Node * const v_kid = ((tb_o_Node * *)tb_l1195->data)[tb_i1195];
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
    while ((({ const char *tb_s1196 = ((tb_o_Node *)tb_nn(v_r, 1603))->q_kind; ((unsigned char)tb_s1196[0] == 105 && (unsigned char)tb_s1196[1] == 110 && (unsigned char)tb_s1196[2] == 100 && (unsigned char)tb_s1196[3] == 101 && (unsigned char)tb_s1196[4] == 120 && tb_s1196[5] == 0); }) || ({ const char *tb_s1197 = ((tb_o_Node *)tb_nn(v_r, 1603))->q_kind; ((unsigned char)tb_s1197[0] == 102 && (unsigned char)tb_s1197[1] == 105 && (unsigned char)tb_s1197[2] == 101 && (unsigned char)tb_s1197[3] == 108 && (unsigned char)tb_s1197[4] == 100 && tb_s1197[5] == 0); }))) {
        v_r = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_r, 1604))->q_kids, 0LL, sizeof(tb_o_Node *), 1604));
    }
    if (({ const char *tb_s1198 = ((tb_o_Node *)tb_nn(v_r, 1605))->q_kind; ((unsigned char)tb_s1198[0] == 110 && (unsigned char)tb_s1198[1] == 97 && (unsigned char)tb_s1198[2] == 109 && (unsigned char)tb_s1198[3] == 101 && tb_s1198[4] == 0); })) {
        return ((tb_o_Node *)tb_nn(v_r, 1606))->q_name;
    }
    return "";
    tb_crash(1601, "функция root_name дошла до конца без return");
    return "";
}

static _Bool f_emit__at_fresh_expr(tb_o_Node * v_n, tb_list * v_inner, tb_list * v_fresh) {
    if (((({ const char *tb_s1199 = ((tb_o_Node *)tb_nn(v_n, 1611))->q_kind; ((unsigned char)tb_s1199[0] == 110 && (unsigned char)tb_s1199[1] == 97 && (unsigned char)tb_s1199[2] == 109 && (unsigned char)tb_s1199[3] == 101 && tb_s1199[4] == 0); }) && f_emit__is_box(((tb_o_Node *)tb_nn(v_n, 1611))->q_ty)) && (!(f_emit__bld_has(v_inner, ((tb_o_Node *)tb_nn(v_n, 1611))->q_name) && f_emit__bld_has(v_fresh, ((tb_o_Node *)tb_nn(v_n, 1611))->q_name))))) {
        return 0;
    }
    {
        tb_list * tb_l1200 = ((tb_o_Node *)tb_nn(v_n, 1613))->q_kids;
        for (long long tb_i1200 = 0; tb_i1200 < tb_l1200->len; tb_i1200++) {
            tb_o_Node * const v_k = ((tb_o_Node * *)tb_l1200->data)[tb_i1200];
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
    if ((((({ const char *tb_s1201 = v_k; ((unsigned char)tb_s1201[0] == 97 && (unsigned char)tb_s1201[1] == 115 && (unsigned char)tb_s1201[2] == 115 && (unsigned char)tb_s1201[3] == 105 && (unsigned char)tb_s1201[4] == 103 && (unsigned char)tb_s1201[5] == 110 && tb_s1201[6] == 0); }) && ((((tb_o_Node *)tb_nn(v_n, 1622))->q_kids)->len == 1LL)) && f_emit__is_box(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1622))->q_kids, 0LL, sizeof(tb_o_Node *), 1622)), 1622))->q_ty)) && (!f_emit__at_fresh_expr((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1622))->q_kids, 0LL, sizeof(tb_o_Node *), 1622)), v_inner, v_fresh)))) {
        v_lose = ((tb_o_Node *)tb_nn(v_n, 1623))->q_name;
    }
    if ((({ const char *tb_s1202 = v_k; ((unsigned char)tb_s1202[0] == 101 && (unsigned char)tb_s1202[1] == 97 && (unsigned char)tb_s1202[2] == 99 && (unsigned char)tb_s1202[3] == 104 && tb_s1202[4] == 0); }) && (!f_emit__at_fresh_expr((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1624))->q_kids, 0LL, sizeof(tb_o_Node *), 1624)), v_inner, v_fresh)))) {
        v_lose = ((tb_o_Node *)tb_nn(v_n, 1625))->q_name;
    }
    if ((((({ const char *tb_s1203 = v_k; ((unsigned char)tb_s1203[0] == 115 && (unsigned char)tb_s1203[1] == 101 && (unsigned char)tb_s1203[2] == 116 && tb_s1203[3] == 0); }) && ({ const char *tb_s1204 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1626))->q_kids, 0LL, sizeof(tb_o_Node *), 1626)), 1626))->q_kind; !((unsigned char)tb_s1204[0] == 110 && (unsigned char)tb_s1204[1] == 97 && (unsigned char)tb_s1204[2] == 109 && (unsigned char)tb_s1204[3] == 101 && tb_s1204[4] == 0); })) && f_emit__is_box(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1626))->q_kids, 1LL, sizeof(tb_o_Node *), 1626)), 1626))->q_ty)) && (!f_emit__at_fresh_expr((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1626))->q_kids, 1LL, sizeof(tb_o_Node *), 1626)), v_inner, v_fresh)))) {
        v_lose = f_emit__root_name((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1627))->q_kids, 0LL, sizeof(tb_o_Node *), 1627)));
    }
    if ((((({ const char *tb_s1205 = v_k; ((unsigned char)tb_s1205[0] == 109 && (unsigned char)tb_s1205[1] == 101 && (unsigned char)tb_s1205[2] == 116 && (unsigned char)tb_s1205[3] == 104 && (unsigned char)tb_s1205[4] == 111 && (unsigned char)tb_s1205[5] == 100 && tb_s1205[6] == 0); }) && ({ const char *tb_s1206 = ((tb_o_Node *)tb_nn(v_n, 1628))->q_name; ((unsigned char)tb_s1206[0] == 97 && (unsigned char)tb_s1206[1] == 100 && (unsigned char)tb_s1206[2] == 100 && tb_s1206[3] == 0); })) && f_emit__is_box(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1628))->q_kids, 1LL, sizeof(tb_o_Node *), 1628)), 1628))->q_kids, 0LL, sizeof(tb_o_Node *), 1628)), 1628))->q_ty)) && (!f_emit__at_fresh_expr((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1628))->q_kids, 1LL, sizeof(tb_o_Node *), 1628)), 1628))->q_kids, 0LL, sizeof(tb_o_Node *), 1628)), v_inner, v_fresh)))) {
        v_lose = f_emit__root_name((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1629))->q_kids, 0LL, sizeof(tb_o_Node *), 1629)));
    }
    if (({ const char *tb_s1207 = v_k; ((unsigned char)tb_s1207[0] == 109 && (unsigned char)tb_s1207[1] == 117 && (unsigned char)tb_s1207[2] == 108 && (unsigned char)tb_s1207[3] == 116 && (unsigned char)tb_s1207[4] == 105 && tb_s1207[5] == 0); })) {
        {
            long long tb_end1208 = ((((tb_o_Node *)tb_nn(v_n, 1631))->q_kids)->len - 2LL);
            for (long long v_i = 0LL; v_i <= tb_end1208; v_i++) {
                if (f_emit__bld_has(v_fresh, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1632))->q_kids, v_i, sizeof(tb_o_Node *), 1632)), 1632))->q_name)) {
                    f_emit__drop_name(v_fresh, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1633))->q_kids, v_i, sizeof(tb_o_Node *), 1633)), 1633))->q_name);
                    v_changed = 1;
                }
            }
        }
    }
    if ((({ const char *tb_s1209 = v_lose; !(tb_s1209[0] == 0); }) && f_emit__bld_has(v_fresh, v_lose))) {
        f_emit__drop_name(v_fresh, v_lose);
        v_changed = 1;
    }
    {
        tb_list * tb_l1210 = ((tb_o_Node *)tb_nn(v_n, 1638))->q_kids;
        for (long long tb_i1210 = 0; tb_i1210 < tb_l1210->len; tb_i1210++) {
            tb_o_Node * const v_kid = ((tb_o_Node * *)tb_l1210->data)[tb_i1210];
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
        tb_list * tb_l1211 = v_names;
        for (long long tb_i1211 = 0; tb_i1211 < tb_l1211->len; tb_i1211++) {
            const char * const v_x = ((const char * *)tb_l1211->data)[tb_i1211];
            if ((strcmp(v_x, v_name) != 0)) {
                { const char * tb_v1212 = v_x; *(const char * *)tb_grow(v_keep) = tb_v1212; }
            }
        }
    }
    while (((v_names)->len > 0LL)) {
        tb_mark tb_am1213 = tb_temp_begin();
        do {
            const char * v_gone = (*(const char * *)tb_pop(v_names, 1648));
        } while (0);
        tb_temp_end(tb_am1213);
    }
    {
        tb_list * tb_l1214 = v_keep;
        for (long long tb_i1214 = 0; tb_i1214 < tb_l1214->len; tb_i1214++) {
            const char * const v_x = ((const char * *)tb_l1214->data)[tb_i1214];
            { const char * tb_v1215 = v_x; *(const char * *)tb_grow(v_names) = tb_v1215; }
        }
    }
}

static _Bool f_emit__at_own(tb_o_Node * v_n, tb_list * v_inner, tb_list * v_fresh) {
    const char * v_r = f_emit__root_name(v_n);
    return ((({ const char *tb_s1216 = v_r; !(tb_s1216[0] == 0); }) && f_emit__bld_has(v_inner, v_r)) && f_emit__bld_has(v_fresh, v_r));
    tb_crash(1653, "функция at_own дошла до конца без return");
    return 0;
}

static _Bool f_emit__at_bad(tb_o_Comp * v_c, tb_o_Node * v_n, tb_list * v_inner, tb_list * v_fresh, long long v_level) {
    const char * v_k = ((tb_o_Node *)tb_nn(v_n, 1659))->q_kind;
    if ((({ const char *tb_s1217 = v_k; ((unsigned char)tb_s1217[0] == 114 && (unsigned char)tb_s1217[1] == 101 && (unsigned char)tb_s1217[2] == 116 && (unsigned char)tb_s1217[3] == 117 && (unsigned char)tb_s1217[4] == 114 && (unsigned char)tb_s1217[5] == 110 && tb_s1217[6] == 0); }) && (!((tb_o_Comp *)tb_nn(v_c, 1660))->q_at_ret_ok))) {
        return 1;
    }
    if (((((({ const char *tb_s1218 = v_k; ((unsigned char)tb_s1218[0] == 102 && (unsigned char)tb_s1218[1] == 97 && (unsigned char)tb_s1218[2] == 105 && (unsigned char)tb_s1218[3] == 108 && tb_s1218[4] == 0); }) || ({ const char *tb_s1219 = v_k; ((unsigned char)tb_s1219[0] == 116 && (unsigned char)tb_s1219[1] == 114 && (unsigned char)tb_s1219[2] == 121 && tb_s1219[3] == 0); })) || (strcmp(v_k, "background") == 0)) || ({ const char *tb_s1220 = v_k; ((unsigned char)tb_s1220[0] == 116 && (unsigned char)tb_s1220[1] == 111 && (unsigned char)tb_s1220[2] == 103 && (unsigned char)tb_s1220[3] == 101 && (unsigned char)tb_s1220[4] == 116 && (unsigned char)tb_s1220[5] == 104 && (unsigned char)tb_s1220[6] == 101 && (unsigned char)tb_s1220[7] == 114 && tb_s1220[8] == 0); })) || ({ const char *tb_s1221 = v_k; ((unsigned char)tb_s1221[0] == 116 && (unsigned char)tb_s1221[1] == 101 && (unsigned char)tb_s1221[2] == 109 && (unsigned char)tb_s1221[3] == 112 && tb_s1221[4] == 0); }))) {
        return 1;
    }
    if ((({ const char *tb_s1222 = v_k; ((unsigned char)tb_s1222[0] == 115 && (unsigned char)tb_s1222[1] == 116 && (unsigned char)tb_s1222[2] == 111 && (unsigned char)tb_s1222[3] == 112 && tb_s1222[4] == 0); }) && (v_level == 0LL))) {
        return 1;
    }
    if (((({ const char *tb_s1223 = v_k; ((unsigned char)tb_s1223[0] == 101 && (unsigned char)tb_s1223[1] == 97 && (unsigned char)tb_s1223[2] == 99 && (unsigned char)tb_s1223[3] == 104 && tb_s1223[4] == 0); }) || ({ const char *tb_s1224 = v_k; ((unsigned char)tb_s1224[0] == 99 && (unsigned char)tb_s1224[1] == 111 && (unsigned char)tb_s1224[2] == 117 && (unsigned char)tb_s1224[3] == 110 && (unsigned char)tb_s1224[4] == 116 && tb_s1224[5] == 0); })) && ((((tb_o_Node *)tb_nn(v_n, 1666))->q_is_par || ((tb_o_Node *)tb_nn(v_n, 1666))->q_by_vec) || ((tb_o_Node *)tb_nn(v_n, 1666))->q_by_piece))) {
        return 1;
    }
    if ((((({ const char *tb_s1225 = v_k; ((unsigned char)tb_s1225[0] == 97 && (unsigned char)tb_s1225[1] == 115 && (unsigned char)tb_s1225[2] == 115 && (unsigned char)tb_s1225[3] == 105 && (unsigned char)tb_s1225[4] == 103 && (unsigned char)tb_s1225[5] == 110 && tb_s1225[6] == 0); }) && (!((tb_o_Node *)tb_nn(v_n, 1668))->q_creates)) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn(v_n, 1668))->q_ty))) && (!f_emit__bld_has(v_inner, ((tb_o_Node *)tb_nn(v_n, 1668))->q_name)))) {
        return 1;
    }
    if (({ const char *tb_s1226 = v_k; ((unsigned char)tb_s1226[0] == 115 && (unsigned char)tb_s1226[1] == 101 && (unsigned char)tb_s1226[2] == 116 && tb_s1226[3] == 0); })) {
        tb_o_Node * v_target = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1671))->q_kids, 0LL, sizeof(tb_o_Node *), 1671));
        if (((({ const char *tb_s1227 = ((tb_o_Node *)tb_nn(v_target, 1672))->q_kind; ((unsigned char)tb_s1227[0] == 110 && (unsigned char)tb_s1227[1] == 97 && (unsigned char)tb_s1227[2] == 109 && (unsigned char)tb_s1227[3] == 101 && tb_s1227[4] == 0); }) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn(v_target, 1672))->q_ty))) && (!f_emit__bld_has(v_inner, ((tb_o_Node *)tb_nn(v_target, 1672))->q_name)))) {
            return 1;
        }
        if ((({ const char *tb_s1228 = ((tb_o_Node *)tb_nn(v_target, 1674))->q_kind; !((unsigned char)tb_s1228[0] == 110 && (unsigned char)tb_s1228[1] == 97 && (unsigned char)tb_s1228[2] == 109 && (unsigned char)tb_s1228[3] == 101 && tb_s1228[4] == 0); }) && (!f_emit__at_own(v_target, v_inner, v_fresh)))) {
            if ((!f_emit__is_plain_ty(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1675))->q_kids, 1LL, sizeof(tb_o_Node *), 1675)), 1675))->q_ty))) {
                return 1;
            }
            if (((({ const char *tb_s1229 = ((tb_o_Node *)tb_nn(v_target, 1677))->q_kind; ((unsigned char)tb_s1229[0] == 105 && (unsigned char)tb_s1229[1] == 110 && (unsigned char)tb_s1229[2] == 100 && (unsigned char)tb_s1229[3] == 101 && (unsigned char)tb_s1229[4] == 120 && tb_s1229[5] == 0); }) && ({ const char *tb_s1230 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 1677))->q_kids, 0LL, sizeof(tb_o_Node *), 1677)), 1677))->q_ty); ((unsigned char)tb_s1230[0] == 100 && (unsigned char)tb_s1230[1] == 105 && (unsigned char)tb_s1230[2] == 99 && (unsigned char)tb_s1230[3] == 116 && tb_s1230[4] == 0); })) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_target, 1677))->q_kids, 1LL, sizeof(tb_o_Node *), 1677)), 1677))->q_ty)))) {
                return 1;
            }
        }
    }
    if ((((({ const char *tb_s1231 = v_k; ((unsigned char)tb_s1231[0] == 109 && (unsigned char)tb_s1231[1] == 101 && (unsigned char)tb_s1231[2] == 116 && (unsigned char)tb_s1231[3] == 104 && (unsigned char)tb_s1231[4] == 111 && (unsigned char)tb_s1231[5] == 100 && tb_s1231[6] == 0); }) && ({ const char *tb_s1232 = ((tb_o_Node *)tb_nn(v_n, 1679))->q_name; ((unsigned char)tb_s1232[0] == 97 && (unsigned char)tb_s1232[1] == 100 && (unsigned char)tb_s1232[2] == 100 && tb_s1232[3] == 0); })) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1679))->q_kids, 1LL, sizeof(tb_o_Node *), 1679)), 1679))->q_kids, 0LL, sizeof(tb_o_Node *), 1679)), 1679))->q_ty))) && (!f_emit__at_own((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1679))->q_kids, 0LL, sizeof(tb_o_Node *), 1679)), v_inner, v_fresh)))) {
        return 1;
    }
    if (({ const char *tb_s1233 = v_k; ((unsigned char)tb_s1233[0] == 109 && (unsigned char)tb_s1233[1] == 117 && (unsigned char)tb_s1233[2] == 108 && (unsigned char)tb_s1233[3] == 116 && (unsigned char)tb_s1233[4] == 105 && tb_s1233[5] == 0); })) {
        {
            long long tb_end1234 = ((((tb_o_Node *)tb_nn(v_n, 1682))->q_kids)->len - 2LL);
            for (long long v_i = 0LL; v_i <= tb_end1234; v_i++) {
                if (((!f_emit__is_plain_ty(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1683))->q_kids, v_i, sizeof(tb_o_Node *), 1683)), 1683))->q_ty)) && (!f_emit__bld_has(v_inner, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_n, 1683))->q_kids, v_i, sizeof(tb_o_Node *), 1683)), 1683))->q_name)))) {
                    return 1;
                }
            }
        }
    }
    if ((({ const char *tb_s1235 = v_k; ((unsigned char)tb_s1235[0] == 108 && (unsigned char)tb_s1235[1] == 105 && (unsigned char)tb_s1235[2] == 110 && (unsigned char)tb_s1235[3] == 107 && (unsigned char)tb_s1235[4] == 100 && (unsigned char)tb_s1235[5] == 101 && (unsigned char)tb_s1235[6] == 99 && (unsigned char)tb_s1235[7] == 108 && tb_s1235[8] == 0); }) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn(v_n, 1685))->q_ty)))) {
        return 1;
    }
    if (((({ const char *tb_s1236 = v_k; ((unsigned char)tb_s1236[0] == 99 && (unsigned char)tb_s1236[1] == 97 && (unsigned char)tb_s1236[2] == 108 && (unsigned char)tb_s1236[3] == 108 && tb_s1236[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1687))->q_has_ref) && ({ const char *tb_s1237 = ((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1687))->q_ref, 1687))->q_kind; ((unsigned char)tb_s1237[0] == 102 && (unsigned char)tb_s1237[1] == 117 && (unsigned char)tb_s1237[2] == 110 && (unsigned char)tb_s1237[3] == 99 && tb_s1237[4] == 0); }))) {
        tb_o_Node * v_params = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(((tb_o_Node *)tb_nn(v_n, 1688))->q_ref, 1688))->q_kids, 0LL, sizeof(tb_o_Node *), 1688));
        {
            long long tb_end1238 = ((((tb_o_Node *)tb_nn(v_n, 1689))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end1238; v_i++) {
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
    if ((((({ const char *tb_s1239 = v_k; ((unsigned char)tb_s1239[0] == 101 && (unsigned char)tb_s1239[1] == 97 && (unsigned char)tb_s1239[2] == 99 && (unsigned char)tb_s1239[3] == 104 && tb_s1239[4] == 0); }) || ({ const char *tb_s1240 = v_k; ((unsigned char)tb_s1240[0] == 99 && (unsigned char)tb_s1240[1] == 111 && (unsigned char)tb_s1240[2] == 117 && (unsigned char)tb_s1240[3] == 110 && (unsigned char)tb_s1240[4] == 116 && tb_s1240[5] == 0); })) || (strcmp(v_k, "repeat_times") == 0)) || (strcmp(v_k, "repeat_while") == 0))) {
        v_next_level = (v_level + 1LL);
    }
    {
        tb_list * tb_l1241 = ((tb_o_Node *)tb_nn(v_n, 1699))->q_kids;
        for (long long tb_i1241 = 0; tb_i1241 < tb_l1241->len; tb_i1241++) {
            tb_o_Node * const v_kid = ((tb_o_Node * *)tb_l1241->data)[tb_i1241];
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
    if (((((((({ const char *tb_s1242 = v_k; ((unsigned char)tb_s1242[0] == 99 && (unsigned char)tb_s1242[1] == 97 && (unsigned char)tb_s1242[2] == 108 && (unsigned char)tb_s1242[3] == 108 && tb_s1242[4] == 0); }) || ({ const char *tb_s1243 = v_k; ((unsigned char)tb_s1243[0] == 98 && (unsigned char)tb_s1243[1] == 105 && (unsigned char)tb_s1243[2] == 110 && (unsigned char)tb_s1243[3] == 111 && (unsigned char)tb_s1243[4] == 112 && tb_s1243[5] == 0); })) || ({ const char *tb_s1244 = v_k; ((unsigned char)tb_s1244[0] == 108 && (unsigned char)tb_s1244[1] == 105 && (unsigned char)tb_s1244[2] == 115 && (unsigned char)tb_s1244[3] == 116 && tb_s1244[4] == 0); })) || ({ const char *tb_s1245 = v_k; ((unsigned char)tb_s1245[0] == 100 && (unsigned char)tb_s1245[1] == 105 && (unsigned char)tb_s1245[2] == 99 && (unsigned char)tb_s1245[3] == 116 && tb_s1245[4] == 0); })) || ({ const char *tb_s1246 = v_k; ((unsigned char)tb_s1246[0] == 109 && (unsigned char)tb_s1246[1] == 101 && (unsigned char)tb_s1246[2] == 116 && (unsigned char)tb_s1246[3] == 104 && (unsigned char)tb_s1246[4] == 111 && (unsigned char)tb_s1246[5] == 100 && tb_s1246[6] == 0); })) && (((tb_o_Node *)tb_nn(v_n, 1707))->q_ty > 0LL)) && (!f_emit__is_plain_ty(((tb_o_Node *)tb_nn(v_n, 1707))->q_ty)))) {
        return 1;
    }
    if ((({ const char *tb_s1247 = v_k; ((unsigned char)tb_s1247[0] == 99 && (unsigned char)tb_s1247[1] == 97 && (unsigned char)tb_s1247[2] == 108 && (unsigned char)tb_s1247[3] == 108 && tb_s1247[4] == 0); }) && ((tb_o_Node *)tb_nn(v_n, 1709))->q_has_ref)) {
        return 1;
    }
    {
        tb_list * tb_l1248 = ((tb_o_Node *)tb_nn(v_n, 1711))->q_kids;
        for (long long tb_i1248 = 0; tb_i1248 < tb_l1248->len; tb_i1248++) {
            tb_o_Node * const v_kid = ((tb_o_Node * *)tb_l1248->data)[tb_i1248];
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
        tb_list * tb_l1249 = v_inner;
        for (long long tb_i1249 = 0; tb_i1249 < tb_l1249->len; tb_i1249++) {
            const char * const v_x = ((const char * *)tb_l1249->data)[tb_i1249];
            { const char * tb_v1250 = v_x; *(const char * *)tb_grow(v_fresh) = tb_v1250; }
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
    if ((({ const char *tb_s1251 = ((tb_o_Node *)tb_nn(v_f, 1731))->q_name; ((unsigned char)tb_s1251[0] == 109 && (unsigned char)tb_s1251[1] == 97 && (unsigned char)tb_s1251[2] == 105 && (unsigned char)tb_s1251[3] == 110 && tb_s1251[4] == 0); }) || (!((({ const char *tb_s1252 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 1731))->q_ty); ((unsigned char)tb_s1252[0] == 110 && (unsigned char)tb_s1252[1] == 111 && (unsigned char)tb_s1252[2] == 110 && (unsigned char)tb_s1252[3] == 101 && tb_s1252[4] == 0); }) || ({ const char *tb_s1253 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 1731))->q_ty); ((unsigned char)tb_s1253[0] == 109 && (unsigned char)tb_s1253[1] == 117 && (unsigned char)tb_s1253[2] == 108 && (unsigned char)tb_s1253[3] == 116 && (unsigned char)tb_s1253[4] == 105 && tb_s1253[5] == 0); })) || f_emit__is_plain_ty(((tb_o_Node *)tb_nn(v_f, 1731))->q_ty))))) {
        return 0;
    }
    {
        tb_list * tb_l1254 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1733))->q_kids, 2LL, sizeof(tb_o_Node *), 1733)), 1733))->q_kids;
        for (long long tb_i1254 = 0; tb_i1254 < tb_l1254->len; tb_i1254++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l1254->data)[tb_i1254];
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
        tb_list * tb_l1255 = v_inner;
        for (long long tb_i1255 = 0; tb_i1255 < tb_l1255->len; tb_i1255++) {
            const char * const v_x = ((const char * *)tb_l1255->data)[tb_i1255];
            { const char * tb_v1256 = v_x; *(const char * *)tb_grow(v_fresh) = tb_v1256; }
        }
    }
    while (f_emit__at_unfresh(v_c, v_body, v_inner, v_fresh)) {
        v_fresh = v_fresh;
    }
    { _Bool tb_v1257 = 1; _Bool *tb_p1257 = &(((tb_o_Comp *)tb_nn(v_c, 1746))->q_at_ret_ok); *tb_p1257 = tb_v1257; }
    _Bool v_bad = f_emit__at_bad(v_c, v_body, v_inner, v_fresh, 0LL);
    { _Bool tb_v1258 = 0; _Bool *tb_p1258 = &(((tb_o_Comp *)tb_nn(v_c, 1748))->q_at_ret_ok); *tb_p1258 = tb_v1258; }
    return (!v_bad);
    tb_crash(1730, "функция func_arena_ok дошла до конца без return");
    return 0;
}

static void f_emit__emit_loop_body(tb_o_Comp * v_c, tb_o_Node * v_body, long long v_depth) {
    if ((!f_emit__auto_temp_ok(v_c, v_body))) {
        f_emit__emit_block(v_c, v_body, v_depth);
        return;
    }
    { long long tb_v1259 = 1LL; long long *tb_p1259 = &(((tb_o_Comp *)tb_nn(v_c, 1756))->q_label); *tb_p1259 += tb_v1259; }
    const char * v_id = ({ const char *tb_c1260_0 = ""; long long tb_c1260_0n = sizeof("") - 1; char tb_c1260_1d[24]; long long tb_c1260_1n; const char *tb_c1260_1 = tb_digits(((tb_o_Comp *)tb_nn(v_c, 1757))->q_label, tb_c1260_1d, &tb_c1260_1n); char *tb_c1260r = tb_mem(tb_c1260_0n + tb_c1260_1n + 1), *tb_c1260rw = tb_c1260r; memcpy(tb_c1260rw, tb_c1260_0, tb_c1260_0n); tb_c1260rw += tb_c1260_0n; memcpy(tb_c1260rw, tb_c1260_1, tb_c1260_1n); tb_c1260rw += tb_c1260_1n; *tb_c1260rw = 0; (const char *)tb_c1260r; });
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, ({ const char *tb_c1261_0 = "tb_mark tb_am"; long long tb_c1261_0n = sizeof("tb_mark tb_am") - 1; const char *tb_c1261_1 = v_id; long long tb_c1261_1n = (long long)strlen(tb_c1261_1); const char *tb_c1261_2 = " = tb_temp_begin();\n"; long long tb_c1261_2n = sizeof(" = tb_temp_begin();\n") - 1; char *tb_c1261r = tb_mem(tb_c1261_0n + tb_c1261_1n + tb_c1261_2n + 1), *tb_c1261rw = tb_c1261r; memcpy(tb_c1261rw, tb_c1261_0, tb_c1261_0n); tb_c1261rw += tb_c1261_0n; memcpy(tb_c1261rw, tb_c1261_1, tb_c1261_1n); tb_c1261rw += tb_c1261_1n; memcpy(tb_c1261rw, tb_c1261_2, tb_c1261_2n); tb_c1261rw += tb_c1261_2n; *tb_c1261rw = 0; (const char *)tb_c1261r; }));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "do {\n");
    f_emit__emit_block(v_c, v_body, (v_depth + 1LL));
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, "} while (0);\n");
    f_emit__indent(v_c, v_depth);
    f_emit__put(v_c, ({ const char *tb_c1262_0 = "tb_temp_end(tb_am"; long long tb_c1262_0n = sizeof("tb_temp_end(tb_am") - 1; const char *tb_c1262_1 = v_id; long long tb_c1262_1n = (long long)strlen(tb_c1262_1); const char *tb_c1262_2 = ");\n"; long long tb_c1262_2n = sizeof(");\n") - 1; char *tb_c1262r = tb_mem(tb_c1262_0n + tb_c1262_1n + tb_c1262_2n + 1), *tb_c1262rw = tb_c1262r; memcpy(tb_c1262rw, tb_c1262_0, tb_c1262_0n); tb_c1262rw += tb_c1262_0n; memcpy(tb_c1262rw, tb_c1262_1, tb_c1262_1n); tb_c1262rw += tb_c1262_1n; memcpy(tb_c1262rw, tb_c1262_2, tb_c1262_2n); tb_c1262rw += tb_c1262_2n; *tb_c1262rw = 0; (const char *)tb_c1262r; }));
}

static void f_emit__emit_func(tb_o_Comp * v_c, tb_o_Node * v_f) {
    { tb_o_Node * tb_v1263 = v_f; tb_o_Node * *tb_p1263 = &(((tb_o_Comp *)tb_nn(v_c, 1770))->q_cur_func); *tb_p1263 = tb_v1263; }
    tb_o_Node * v_outputs = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1771))->q_kids, 2LL, sizeof(tb_o_Node *), 1771));
    f_emit__put(v_c, "\n");
    f_emit__emit_signature(v_c, v_f);
    f_emit__put(v_c, " {\n");
    {
        tb_list * tb_l1264 = ((tb_o_Node *)tb_nn(v_outputs, 1775))->q_kids;
        for (long long tb_i1264 = 0; tb_i1264 < tb_l1264->len; tb_i1264++) {
            tb_o_Node * const v_o = ((tb_o_Node * *)tb_l1264->data)[tb_i1264];
            if ((!((tb_o_Node *)tb_nn(v_o, 1776))->q_is_const)) {
                f_emit__put(v_c, ({ const char *tb_c1265_0 = "    "; long long tb_c1265_0n = sizeof("    ") - 1; const char *tb_c1265_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_o, 1777))->q_ty); long long tb_c1265_1n = (long long)strlen(tb_c1265_1); const char *tb_c1265_2 = " v_"; long long tb_c1265_2n = sizeof(" v_") - 1; const char *tb_c1265_3 = ((tb_o_Node *)tb_nn(v_o, 1777))->q_name; long long tb_c1265_3n = (long long)strlen(tb_c1265_3); const char *tb_c1265_4 = " = "; long long tb_c1265_4n = sizeof(" = ") - 1; char *tb_c1265r = tb_mem(tb_c1265_0n + tb_c1265_1n + tb_c1265_2n + tb_c1265_3n + tb_c1265_4n + 1), *tb_c1265rw = tb_c1265r; memcpy(tb_c1265rw, tb_c1265_0, tb_c1265_0n); tb_c1265rw += tb_c1265_0n; memcpy(tb_c1265rw, tb_c1265_1, tb_c1265_1n); tb_c1265rw += tb_c1265_1n; memcpy(tb_c1265rw, tb_c1265_2, tb_c1265_2n); tb_c1265rw += tb_c1265_2n; memcpy(tb_c1265rw, tb_c1265_3, tb_c1265_3n); tb_c1265rw += tb_c1265_3n; memcpy(tb_c1265rw, tb_c1265_4, tb_c1265_4n); tb_c1265rw += tb_c1265_4n; *tb_c1265rw = 0; (const char *)tb_c1265r; }));
                f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_o, 1778))->q_ty);
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__find_builders(v_c, v_f);
    {
        tb_list * tb_l1266 = ((tb_o_Comp *)tb_nn(v_c, 1781))->q_builders;
        for (long long tb_i1266 = 0; tb_i1266 < tb_l1266->len; tb_i1266++) {
            const char * const v_b = ((const char * *)tb_l1266->data)[tb_i1266];
            f_emit__put(v_c, ({ const char *tb_c1267_0 = "    long long tb_rl_"; long long tb_c1267_0n = sizeof("    long long tb_rl_") - 1; const char *tb_c1267_1 = v_b; long long tb_c1267_1n = (long long)strlen(tb_c1267_1); const char *tb_c1267_2 = " = -1, tb_rc_"; long long tb_c1267_2n = sizeof(" = -1, tb_rc_") - 1; const char *tb_c1267_3 = v_b; long long tb_c1267_3n = (long long)strlen(tb_c1267_3); const char *tb_c1267_4 = " = 0; (void)tb_rl_"; long long tb_c1267_4n = sizeof(" = 0; (void)tb_rl_") - 1; const char *tb_c1267_5 = v_b; long long tb_c1267_5n = (long long)strlen(tb_c1267_5); const char *tb_c1267_6 = "; (void)tb_rc_"; long long tb_c1267_6n = sizeof("; (void)tb_rc_") - 1; const char *tb_c1267_7 = v_b; long long tb_c1267_7n = (long long)strlen(tb_c1267_7); const char *tb_c1267_8 = ";\n"; long long tb_c1267_8n = sizeof(";\n") - 1; char *tb_c1267r = tb_mem(tb_c1267_0n + tb_c1267_1n + tb_c1267_2n + tb_c1267_3n + tb_c1267_4n + tb_c1267_5n + tb_c1267_6n + tb_c1267_7n + tb_c1267_8n + 1), *tb_c1267rw = tb_c1267r; memcpy(tb_c1267rw, tb_c1267_0, tb_c1267_0n); tb_c1267rw += tb_c1267_0n; memcpy(tb_c1267rw, tb_c1267_1, tb_c1267_1n); tb_c1267rw += tb_c1267_1n; memcpy(tb_c1267rw, tb_c1267_2, tb_c1267_2n); tb_c1267rw += tb_c1267_2n; memcpy(tb_c1267rw, tb_c1267_3, tb_c1267_3n); tb_c1267rw += tb_c1267_3n; memcpy(tb_c1267rw, tb_c1267_4, tb_c1267_4n); tb_c1267rw += tb_c1267_4n; memcpy(tb_c1267rw, tb_c1267_5, tb_c1267_5n); tb_c1267rw += tb_c1267_5n; memcpy(tb_c1267rw, tb_c1267_6, tb_c1267_6n); tb_c1267rw += tb_c1267_6n; memcpy(tb_c1267rw, tb_c1267_7, tb_c1267_7n); tb_c1267rw += tb_c1267_7n; memcpy(tb_c1267rw, tb_c1267_8, tb_c1267_8n); tb_c1267rw += tb_c1267_8n; *tb_c1267rw = 0; (const char *)tb_c1267r; }));
        }
    }
    { const char * tb_v1268 = ""; const char * *tb_p1268 = &(((tb_o_Comp *)tb_nn(v_c, 1783))->q_fmark); *tb_p1268 = tb_v1268; }
    if (f_emit__func_arena_ok(v_c, v_f)) {
        { const char * tb_v1269 = "tb_fm"; const char * *tb_p1269 = &(((tb_o_Comp *)tb_nn(v_c, 1787))->q_fmark); *tb_p1269 = tb_v1269; }
        f_emit__put(v_c, "    int tb_fa = tb_temp_depth == 0; tb_mark tb_fm = {0, 0}; if (tb_fa) tb_fm = tb_temp_begin();\n");
    }
    f_emit__emit_block(v_c, (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_f, 1789))->q_kids, 1LL, sizeof(tb_o_Node *), 1789)), 1LL);
    tb_list * v_no_builders = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1270 = v_no_builders; tb_list * *tb_p1270 = &(((tb_o_Comp *)tb_nn(v_c, 1791))->q_builders); *tb_p1270 = tb_v1270; }
    if (({ const char *tb_s1271 = ((tb_o_Comp *)tb_nn(v_c, 1792))->q_fmark; !(tb_s1271[0] == 0); })) {
        f_emit__put(v_c, "    if (tb_fa) tb_temp_end(tb_fm);\n");
    }
    { const char * tb_v1272 = ""; const char * *tb_p1272 = &(((tb_o_Comp *)tb_nn(v_c, 1794))->q_fmark); *tb_p1272 = tb_v1272; }
    if (((((tb_o_Node *)tb_nn(v_outputs, 1795))->q_kids)->len > 0LL)) {
        f_emit__emit_gives_return(v_c, 1LL);
    } else if (({ const char *tb_s1273 = f_check__kind_of(v_c, ((tb_o_Node *)tb_nn(v_f, 1797))->q_ty); !((unsigned char)tb_s1273[0] == 110 && (unsigned char)tb_s1273[1] == 111 && (unsigned char)tb_s1273[2] == 110 && (unsigned char)tb_s1273[3] == 101 && tb_s1273[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c1274_0 = "    tb_crash("; long long tb_c1274_0n = sizeof("    tb_crash(") - 1; char tb_c1274_1d[24]; long long tb_c1274_1n; const char *tb_c1274_1 = tb_digits(((tb_o_Node *)tb_nn(v_f, 1798))->q_line, tb_c1274_1d, &tb_c1274_1n); const char *tb_c1274_2 = ", \"функция "; long long tb_c1274_2n = sizeof(", \"функция ") - 1; const char *tb_c1274_3 = ((tb_o_Node *)tb_nn(v_f, 1798))->q_name; long long tb_c1274_3n = (long long)strlen(tb_c1274_3); const char *tb_c1274_4 = " дошла до конца без return\");\n"; long long tb_c1274_4n = sizeof(" дошла до конца без return\");\n") - 1; char *tb_c1274r = tb_mem(tb_c1274_0n + tb_c1274_1n + tb_c1274_2n + tb_c1274_3n + tb_c1274_4n + 1), *tb_c1274rw = tb_c1274r; memcpy(tb_c1274rw, tb_c1274_0, tb_c1274_0n); tb_c1274rw += tb_c1274_0n; memcpy(tb_c1274rw, tb_c1274_1, tb_c1274_1n); tb_c1274rw += tb_c1274_1n; memcpy(tb_c1274rw, tb_c1274_2, tb_c1274_2n); tb_c1274rw += tb_c1274_2n; memcpy(tb_c1274rw, tb_c1274_3, tb_c1274_3n); tb_c1274rw += tb_c1274_3n; memcpy(tb_c1274rw, tb_c1274_4, tb_c1274_4n); tb_c1274rw += tb_c1274_4n; *tb_c1274rw = 0; (const char *)tb_c1274r; }));
        f_emit__put(v_c, "    return ");
        f_emit__emit_default(v_c, ((tb_o_Node *)tb_nn(v_f, 1800))->q_ty);
        f_emit__put(v_c, ";\n");
    }
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_show(tb_o_Comp * v_c, tb_o_Type * v_t) {
    const char * v_k = ((tb_o_Type *)tb_nn(v_t, 1806))->q_kind;
    if (((({ const char *tb_s1275 = v_k; ((unsigned char)tb_s1275[0] == 110 && (unsigned char)tb_s1275[1] == 111 && (unsigned char)tb_s1275[2] == 110 && (unsigned char)tb_s1275[3] == 101 && tb_s1275[4] == 0); }) || ({ const char *tb_s1276 = v_k; ((unsigned char)tb_s1276[0] == 109 && (unsigned char)tb_s1276[1] == 117 && (unsigned char)tb_s1276[2] == 108 && (unsigned char)tb_s1276[3] == 116 && (unsigned char)tb_s1276[4] == 105 && tb_s1276[5] == 0); })) || ({ const char *tb_s1277 = v_k; ((unsigned char)tb_s1277[0] == 117 && (unsigned char)tb_s1277[1] == 110 && (unsigned char)tb_s1277[2] == 107 && (unsigned char)tb_s1277[3] == 110 && (unsigned char)tb_s1277[4] == 111 && (unsigned char)tb_s1277[5] == 119 && (unsigned char)tb_s1277[6] == 110 && tb_s1277[7] == 0); }))) {
        return;
    }
    if (({ const char *tb_s1278 = v_k; ((unsigned char)tb_s1278[0] == 116 && (unsigned char)tb_s1278[1] == 97 && (unsigned char)tb_s1278[2] == 115 && (unsigned char)tb_s1278[3] == 107 && tb_s1278[4] == 0); })) {
        f_emit__put(v_c, ({ const char *tb_c1279_0 = "static void tb_show_"; long long tb_c1279_0n = sizeof("static void tb_show_") - 1; char tb_c1279_1d[24]; long long tb_c1279_1n; const char *tb_c1279_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1810))->q_id, tb_c1279_1d, &tb_c1279_1n); const char *tb_c1279_2 = "(tb_task *x) { (void)x; tb_ps(\"<background>\"); }\n"; long long tb_c1279_2n = sizeof("(tb_task *x) { (void)x; tb_ps(\"<background>\"); }\n") - 1; char *tb_c1279r = tb_mem(tb_c1279_0n + tb_c1279_1n + tb_c1279_2n + 1), *tb_c1279rw = tb_c1279r; memcpy(tb_c1279rw, tb_c1279_0, tb_c1279_0n); tb_c1279rw += tb_c1279_0n; memcpy(tb_c1279rw, tb_c1279_1, tb_c1279_1n); tb_c1279rw += tb_c1279_1n; memcpy(tb_c1279rw, tb_c1279_2, tb_c1279_2n); tb_c1279rw += tb_c1279_2n; *tb_c1279rw = 0; (const char *)tb_c1279r; }));
        return;
    }
    f_emit__put(v_c, ({ const char *tb_c1280_0 = "static void tb_show_"; long long tb_c1280_0n = sizeof("static void tb_show_") - 1; char tb_c1280_1d[24]; long long tb_c1280_1n; const char *tb_c1280_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1812))->q_id, tb_c1280_1d, &tb_c1280_1n); const char *tb_c1280_2 = "("; long long tb_c1280_2n = sizeof("(") - 1; const char *tb_c1280_3 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1812))->q_id); long long tb_c1280_3n = (long long)strlen(tb_c1280_3); const char *tb_c1280_4 = " x) {\n"; long long tb_c1280_4n = sizeof(" x) {\n") - 1; char *tb_c1280r = tb_mem(tb_c1280_0n + tb_c1280_1n + tb_c1280_2n + tb_c1280_3n + tb_c1280_4n + 1), *tb_c1280rw = tb_c1280r; memcpy(tb_c1280rw, tb_c1280_0, tb_c1280_0n); tb_c1280rw += tb_c1280_0n; memcpy(tb_c1280rw, tb_c1280_1, tb_c1280_1n); tb_c1280rw += tb_c1280_1n; memcpy(tb_c1280rw, tb_c1280_2, tb_c1280_2n); tb_c1280rw += tb_c1280_2n; memcpy(tb_c1280rw, tb_c1280_3, tb_c1280_3n); tb_c1280rw += tb_c1280_3n; memcpy(tb_c1280rw, tb_c1280_4, tb_c1280_4n); tb_c1280rw += tb_c1280_4n; *tb_c1280rw = 0; (const char *)tb_c1280r; }));
    if (({ const char *tb_s1281 = v_k; ((unsigned char)tb_s1281[0] == 105 && (unsigned char)tb_s1281[1] == 110 && (unsigned char)tb_s1281[2] == 116 && tb_s1281[3] == 0); })) {
        f_emit__put(v_c, "    printf(\"%lld\", x);\n");
    } else if (({ const char *tb_s1282 = v_k; ((unsigned char)tb_s1282[0] == 100 && (unsigned char)tb_s1282[1] == 101 && (unsigned char)tb_s1282[2] == 99 && tb_s1282[3] == 0); })) {
        f_emit__put(v_c, "    printf(\"%g\", x);\n");
    } else if (({ const char *tb_s1283 = v_k; ((unsigned char)tb_s1283[0] == 98 && (unsigned char)tb_s1283[1] == 111 && (unsigned char)tb_s1283[2] == 111 && (unsigned char)tb_s1283[3] == 108 && tb_s1283[4] == 0); })) {
        f_emit__put(v_c, "    tb_ps(x ? \"true\" : \"false\");\n");
    } else if (({ const char *tb_s1284 = v_k; ((unsigned char)tb_s1284[0] == 116 && (unsigned char)tb_s1284[1] == 101 && (unsigned char)tb_s1284[2] == 120 && (unsigned char)tb_s1284[3] == 116 && tb_s1284[4] == 0); })) {
        f_emit__put(v_c, "    tb_ps(x);\n");
    } else if (({ const char *tb_s1285 = v_k; ((unsigned char)tb_s1285[0] == 108 && (unsigned char)tb_s1285[1] == 105 && (unsigned char)tb_s1285[2] == 115 && (unsigned char)tb_s1285[3] == 116 && tb_s1285[4] == 0); })) {
        const char * v_elem = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1822))->q_elem);
        f_emit__put(v_c, "    tb_ps(\"[\");\n");
        f_emit__put(v_c, "    for (long long i = 0; i < x->len; i++) {\n");
        f_emit__put(v_c, "        if (i > 0) tb_ps(\", \");\n");
        if (f_check__is_cols_obj(v_c, ((tb_o_Type *)tb_nn(v_t, 1826))->q_elem)) {
            f_emit__put(v_c, ({ const char *tb_c1286_0 = "        tb_show_"; long long tb_c1286_0n = sizeof("        tb_show_") - 1; char tb_c1286_1d[24]; long long tb_c1286_1n; const char *tb_c1286_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1827))->q_elem, tb_c1286_1d, &tb_c1286_1n); const char *tb_c1286_2 = "(("; long long tb_c1286_2n = sizeof("((") - 1; const char *tb_c1286_3 = v_elem; long long tb_c1286_3n = (long long)strlen(tb_c1286_3); const char *tb_c1286_4 = "){x, i});\n"; long long tb_c1286_4n = sizeof("){x, i});\n") - 1; char *tb_c1286r = tb_mem(tb_c1286_0n + tb_c1286_1n + tb_c1286_2n + tb_c1286_3n + tb_c1286_4n + 1), *tb_c1286rw = tb_c1286r; memcpy(tb_c1286rw, tb_c1286_0, tb_c1286_0n); tb_c1286rw += tb_c1286_0n; memcpy(tb_c1286rw, tb_c1286_1, tb_c1286_1n); tb_c1286rw += tb_c1286_1n; memcpy(tb_c1286rw, tb_c1286_2, tb_c1286_2n); tb_c1286rw += tb_c1286_2n; memcpy(tb_c1286rw, tb_c1286_3, tb_c1286_3n); tb_c1286rw += tb_c1286_3n; memcpy(tb_c1286rw, tb_c1286_4, tb_c1286_4n); tb_c1286rw += tb_c1286_4n; *tb_c1286rw = 0; (const char *)tb_c1286r; }));
        } else if ((((tb_o_Type *)tb_nn(v_t, 1828))->q_elem == 3LL)) {
            f_emit__put(v_c, "        tb_show_3(TB_BIT(x, i));\n");
        } else {
            f_emit__put(v_c, ({ const char *tb_c1287_0 = "        tb_show_"; long long tb_c1287_0n = sizeof("        tb_show_") - 1; char tb_c1287_1d[24]; long long tb_c1287_1n; const char *tb_c1287_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1831))->q_elem, tb_c1287_1d, &tb_c1287_1n); const char *tb_c1287_2 = "((("; long long tb_c1287_2n = sizeof("(((") - 1; const char *tb_c1287_3 = v_elem; long long tb_c1287_3n = (long long)strlen(tb_c1287_3); const char *tb_c1287_4 = " *)x->data)[i]);\n"; long long tb_c1287_4n = sizeof(" *)x->data)[i]);\n") - 1; char *tb_c1287r = tb_mem(tb_c1287_0n + tb_c1287_1n + tb_c1287_2n + tb_c1287_3n + tb_c1287_4n + 1), *tb_c1287rw = tb_c1287r; memcpy(tb_c1287rw, tb_c1287_0, tb_c1287_0n); tb_c1287rw += tb_c1287_0n; memcpy(tb_c1287rw, tb_c1287_1, tb_c1287_1n); tb_c1287rw += tb_c1287_1n; memcpy(tb_c1287rw, tb_c1287_2, tb_c1287_2n); tb_c1287rw += tb_c1287_2n; memcpy(tb_c1287rw, tb_c1287_3, tb_c1287_3n); tb_c1287rw += tb_c1287_3n; memcpy(tb_c1287rw, tb_c1287_4, tb_c1287_4n); tb_c1287rw += tb_c1287_4n; *tb_c1287rw = 0; (const char *)tb_c1287r; }));
        }
        f_emit__put(v_c, "    }\n");
        f_emit__put(v_c, "    tb_ps(\"]\");\n");
    } else if (({ const char *tb_s1288 = v_k; ((unsigned char)tb_s1288[0] == 100 && (unsigned char)tb_s1288[1] == 105 && (unsigned char)tb_s1288[2] == 99 && (unsigned char)tb_s1288[3] == 116 && tb_s1288[4] == 0); })) {
        const char * v_kt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1835))->q_key);
        const char * v_vt = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 1836))->q_elem);
        f_emit__put(v_c, "    tb_ps(\"{\");\n");
        f_emit__put(v_c, "    for (long long i = 0; i < x->keys->len; i++) {\n");
        f_emit__put(v_c, "        if (i > 0) tb_ps(\", \");\n");
        f_emit__put(v_c, ({ const char *tb_c1289_0 = "        tb_show_"; long long tb_c1289_0n = sizeof("        tb_show_") - 1; char tb_c1289_1d[24]; long long tb_c1289_1n; const char *tb_c1289_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1840))->q_key, tb_c1289_1d, &tb_c1289_1n); const char *tb_c1289_2 = "((("; long long tb_c1289_2n = sizeof("(((") - 1; const char *tb_c1289_3 = v_kt; long long tb_c1289_3n = (long long)strlen(tb_c1289_3); const char *tb_c1289_4 = " *)x->keys->data)[i]);\n"; long long tb_c1289_4n = sizeof(" *)x->keys->data)[i]);\n") - 1; char *tb_c1289r = tb_mem(tb_c1289_0n + tb_c1289_1n + tb_c1289_2n + tb_c1289_3n + tb_c1289_4n + 1), *tb_c1289rw = tb_c1289r; memcpy(tb_c1289rw, tb_c1289_0, tb_c1289_0n); tb_c1289rw += tb_c1289_0n; memcpy(tb_c1289rw, tb_c1289_1, tb_c1289_1n); tb_c1289rw += tb_c1289_1n; memcpy(tb_c1289rw, tb_c1289_2, tb_c1289_2n); tb_c1289rw += tb_c1289_2n; memcpy(tb_c1289rw, tb_c1289_3, tb_c1289_3n); tb_c1289rw += tb_c1289_3n; memcpy(tb_c1289rw, tb_c1289_4, tb_c1289_4n); tb_c1289rw += tb_c1289_4n; *tb_c1289rw = 0; (const char *)tb_c1289r; }));
        f_emit__put(v_c, "        tb_ps(\": \");\n");
        f_emit__put(v_c, ({ const char *tb_c1290_0 = "        tb_show_"; long long tb_c1290_0n = sizeof("        tb_show_") - 1; char tb_c1290_1d[24]; long long tb_c1290_1n; const char *tb_c1290_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 1842))->q_elem, tb_c1290_1d, &tb_c1290_1n); const char *tb_c1290_2 = "((("; long long tb_c1290_2n = sizeof("(((") - 1; const char *tb_c1290_3 = v_vt; long long tb_c1290_3n = (long long)strlen(tb_c1290_3); const char *tb_c1290_4 = " *)x->vals->data)[i]);\n"; long long tb_c1290_4n = sizeof(" *)x->vals->data)[i]);\n") - 1; char *tb_c1290r = tb_mem(tb_c1290_0n + tb_c1290_1n + tb_c1290_2n + tb_c1290_3n + tb_c1290_4n + 1), *tb_c1290rw = tb_c1290r; memcpy(tb_c1290rw, tb_c1290_0, tb_c1290_0n); tb_c1290rw += tb_c1290_0n; memcpy(tb_c1290rw, tb_c1290_1, tb_c1290_1n); tb_c1290rw += tb_c1290_1n; memcpy(tb_c1290rw, tb_c1290_2, tb_c1290_2n); tb_c1290rw += tb_c1290_2n; memcpy(tb_c1290rw, tb_c1290_3, tb_c1290_3n); tb_c1290rw += tb_c1290_3n; memcpy(tb_c1290rw, tb_c1290_4, tb_c1290_4n); tb_c1290rw += tb_c1290_4n; *tb_c1290rw = 0; (const char *)tb_c1290r; }));
        f_emit__put(v_c, "    }\n");
        f_emit__put(v_c, "    tb_ps(\"}\");\n");
    } else if (({ const char *tb_s1291 = v_k; ((unsigned char)tb_s1291[0] == 111 && (unsigned char)tb_s1291[1] == 98 && (unsigned char)tb_s1291[2] == 106 && tb_s1291[3] == 0); })) {
        tb_o_Node * v_o = ((tb_o_Type *)tb_nn(v_t, 1846))->q_obj_node;
        if (((tb_o_Node *)tb_nn(v_o, 1847))->q_by_cols) {
            f_emit__put(v_c, "    if (x.c == 0) { tb_ps(\"пусто\"); return; }\n");
        } else {
            f_emit__put(v_c, "    if (x == 0) { tb_ps(\"пусто\"); return; }\n");
        }
        f_emit__put(v_c, ({ const char *tb_c1292_0 = "    tb_ps(\""; long long tb_c1292_0n = sizeof("    tb_ps(\"") - 1; const char *tb_c1292_1 = ((tb_o_Node *)tb_nn(v_o, 1851))->q_name; long long tb_c1292_1n = (long long)strlen(tb_c1292_1); const char *tb_c1292_2 = "(\");\n"; long long tb_c1292_2n = sizeof("(\");\n") - 1; char *tb_c1292r = tb_mem(tb_c1292_0n + tb_c1292_1n + tb_c1292_2n + 1), *tb_c1292rw = tb_c1292r; memcpy(tb_c1292rw, tb_c1292_0, tb_c1292_0n); tb_c1292rw += tb_c1292_0n; memcpy(tb_c1292rw, tb_c1292_1, tb_c1292_1n); tb_c1292rw += tb_c1292_1n; memcpy(tb_c1292rw, tb_c1292_2, tb_c1292_2n); tb_c1292rw += tb_c1292_2n; *tb_c1292rw = 0; (const char *)tb_c1292r; }));
        {
            long long tb_end1293 = ((((tb_o_Node *)tb_nn(v_o, 1852))->q_kids)->len - 1LL);
            for (long long v_i = 0LL; v_i <= tb_end1293; v_i++) {
                const char * v_sep = "";
                if ((v_i > 0LL)) {
                    v_sep = ", ";
                }
                f_emit__put(v_c, ({ const char *tb_c1294_0 = "    tb_ps(\""; long long tb_c1294_0n = sizeof("    tb_ps(\"") - 1; const char *tb_c1294_1 = v_sep; long long tb_c1294_1n = (long long)strlen(tb_c1294_1); const char *tb_c1294_2 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1856))->q_kids, v_i, sizeof(tb_o_Node *), 1856)), 1856))->q_name; long long tb_c1294_2n = (long long)strlen(tb_c1294_2); const char *tb_c1294_3 = ": \");\n"; long long tb_c1294_3n = sizeof(": \");\n") - 1; char *tb_c1294r = tb_mem(tb_c1294_0n + tb_c1294_1n + tb_c1294_2n + tb_c1294_3n + 1), *tb_c1294rw = tb_c1294r; memcpy(tb_c1294rw, tb_c1294_0, tb_c1294_0n); tb_c1294rw += tb_c1294_0n; memcpy(tb_c1294rw, tb_c1294_1, tb_c1294_1n); tb_c1294rw += tb_c1294_1n; memcpy(tb_c1294rw, tb_c1294_2, tb_c1294_2n); tb_c1294rw += tb_c1294_2n; memcpy(tb_c1294rw, tb_c1294_3, tb_c1294_3n); tb_c1294rw += tb_c1294_3n; *tb_c1294rw = 0; (const char *)tb_c1294r; }));
                if (((tb_o_Node *)tb_nn(v_o, 1857))->q_by_cols) {
                    f_emit__put(v_c, ({ const char *tb_c1295_0 = "    tb_show_"; long long tb_c1295_0n = sizeof("    tb_show_") - 1; char tb_c1295_1d[24]; long long tb_c1295_1n; const char *tb_c1295_1 = tb_digits(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1858))->q_kids, v_i, sizeof(tb_o_Node *), 1858)), 1858))->q_ty, tb_c1295_1d, &tb_c1295_1n); const char *tb_c1295_2 = "(x.c->q_"; long long tb_c1295_2n = sizeof("(x.c->q_") - 1; const char *tb_c1295_3 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1858))->q_kids, v_i, sizeof(tb_o_Node *), 1858)), 1858))->q_name; long long tb_c1295_3n = (long long)strlen(tb_c1295_3); const char *tb_c1295_4 = "[x.i]);\n"; long long tb_c1295_4n = sizeof("[x.i]);\n") - 1; char *tb_c1295r = tb_mem(tb_c1295_0n + tb_c1295_1n + tb_c1295_2n + tb_c1295_3n + tb_c1295_4n + 1), *tb_c1295rw = tb_c1295r; memcpy(tb_c1295rw, tb_c1295_0, tb_c1295_0n); tb_c1295rw += tb_c1295_0n; memcpy(tb_c1295rw, tb_c1295_1, tb_c1295_1n); tb_c1295rw += tb_c1295_1n; memcpy(tb_c1295rw, tb_c1295_2, tb_c1295_2n); tb_c1295rw += tb_c1295_2n; memcpy(tb_c1295rw, tb_c1295_3, tb_c1295_3n); tb_c1295rw += tb_c1295_3n; memcpy(tb_c1295rw, tb_c1295_4, tb_c1295_4n); tb_c1295rw += tb_c1295_4n; *tb_c1295rw = 0; (const char *)tb_c1295r; }));
                } else {
                    f_emit__put(v_c, ({ const char *tb_c1296_0 = "    tb_show_"; long long tb_c1296_0n = sizeof("    tb_show_") - 1; char tb_c1296_1d[24]; long long tb_c1296_1n; const char *tb_c1296_1 = tb_digits(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1860))->q_kids, v_i, sizeof(tb_o_Node *), 1860)), 1860))->q_ty, tb_c1296_1d, &tb_c1296_1n); const char *tb_c1296_2 = "(x"; long long tb_c1296_2n = sizeof("(x") - 1; const char *tb_c1296_3 = f_emit__field_path(v_o, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1860))->q_kids, v_i, sizeof(tb_o_Node *), 1860)), 1860))->q_name); long long tb_c1296_3n = (long long)strlen(tb_c1296_3); const char *tb_c1296_4 = ");\n"; long long tb_c1296_4n = sizeof(");\n") - 1; char *tb_c1296r = tb_mem(tb_c1296_0n + tb_c1296_1n + tb_c1296_2n + tb_c1296_3n + tb_c1296_4n + 1), *tb_c1296rw = tb_c1296r; memcpy(tb_c1296rw, tb_c1296_0, tb_c1296_0n); tb_c1296rw += tb_c1296_0n; memcpy(tb_c1296rw, tb_c1296_1, tb_c1296_1n); tb_c1296rw += tb_c1296_1n; memcpy(tb_c1296rw, tb_c1296_2, tb_c1296_2n); tb_c1296rw += tb_c1296_2n; memcpy(tb_c1296rw, tb_c1296_3, tb_c1296_3n); tb_c1296rw += tb_c1296_3n; memcpy(tb_c1296rw, tb_c1296_4, tb_c1296_4n); tb_c1296rw += tb_c1296_4n; *tb_c1296rw = 0; (const char *)tb_c1296r; }));
                }
            }
        }
        f_emit__put(v_c, "    tb_ps(\")\");\n");
    }
    f_emit__put(v_c, "}\n");
}

static void f_emit__emit_cols_obj(tb_o_Comp * v_c, tb_o_Node * v_o) {
    f_emit__put(v_c, ({ const char *tb_c1297_0 = "struct tb_cols_"; long long tb_c1297_0n = sizeof("struct tb_cols_") - 1; const char *tb_c1297_1 = ((tb_o_Node *)tb_nn(v_o, 1868))->q_name; long long tb_c1297_1n = (long long)strlen(tb_c1297_1); const char *tb_c1297_2 = " {\n    long long len, cap;\n"; long long tb_c1297_2n = sizeof(" {\n    long long len, cap;\n") - 1; char *tb_c1297r = tb_mem(tb_c1297_0n + tb_c1297_1n + tb_c1297_2n + 1), *tb_c1297rw = tb_c1297r; memcpy(tb_c1297rw, tb_c1297_0, tb_c1297_0n); tb_c1297rw += tb_c1297_0n; memcpy(tb_c1297rw, tb_c1297_1, tb_c1297_1n); tb_c1297rw += tb_c1297_1n; memcpy(tb_c1297rw, tb_c1297_2, tb_c1297_2n); tb_c1297rw += tb_c1297_2n; *tb_c1297rw = 0; (const char *)tb_c1297r; }));
    {
        tb_list * tb_l1298 = ((tb_o_Node *)tb_nn(v_o, 1869))->q_kids;
        for (long long tb_i1298 = 0; tb_i1298 < tb_l1298->len; tb_i1298++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1298->data)[tb_i1298];
            f_emit__put(v_c, ({ const char *tb_c1299_0 = "    "; long long tb_c1299_0n = sizeof("    ") - 1; const char *tb_c1299_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1870))->q_ty); long long tb_c1299_1n = (long long)strlen(tb_c1299_1); const char *tb_c1299_2 = " *q_"; long long tb_c1299_2n = sizeof(" *q_") - 1; const char *tb_c1299_3 = ((tb_o_Node *)tb_nn(v_f, 1870))->q_name; long long tb_c1299_3n = (long long)strlen(tb_c1299_3); const char *tb_c1299_4 = ";\n"; long long tb_c1299_4n = sizeof(";\n") - 1; char *tb_c1299r = tb_mem(tb_c1299_0n + tb_c1299_1n + tb_c1299_2n + tb_c1299_3n + tb_c1299_4n + 1), *tb_c1299rw = tb_c1299r; memcpy(tb_c1299rw, tb_c1299_0, tb_c1299_0n); tb_c1299rw += tb_c1299_0n; memcpy(tb_c1299rw, tb_c1299_1, tb_c1299_1n); tb_c1299rw += tb_c1299_1n; memcpy(tb_c1299rw, tb_c1299_2, tb_c1299_2n); tb_c1299rw += tb_c1299_2n; memcpy(tb_c1299rw, tb_c1299_3, tb_c1299_3n); tb_c1299rw += tb_c1299_3n; memcpy(tb_c1299rw, tb_c1299_4, tb_c1299_4n); tb_c1299rw += tb_c1299_4n; *tb_c1299rw = 0; (const char *)tb_c1299r; }));
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
        f_emit__put(v_c, ({ const char *tb_c1300_0 = "struct tb_c_"; long long tb_c1300_0n = sizeof("struct tb_c_") - 1; const char *tb_c1300_1 = ((tb_o_Node *)tb_nn(v_o, 1879))->q_name; long long tb_c1300_1n = (long long)strlen(tb_c1300_1); const char *tb_c1300_2 = " {\n"; long long tb_c1300_2n = sizeof(" {\n") - 1; char *tb_c1300r = tb_mem(tb_c1300_0n + tb_c1300_1n + tb_c1300_2n + 1), *tb_c1300rw = tb_c1300r; memcpy(tb_c1300rw, tb_c1300_0, tb_c1300_0n); tb_c1300rw += tb_c1300_0n; memcpy(tb_c1300rw, tb_c1300_1, tb_c1300_1n); tb_c1300rw += tb_c1300_1n; memcpy(tb_c1300rw, tb_c1300_2, tb_c1300_2n); tb_c1300rw += tb_c1300_2n; *tb_c1300rw = 0; (const char *)tb_c1300r; }));
        {
            tb_list * tb_l1301 = ((tb_o_Node *)tb_nn(v_o, 1880))->q_kids;
            for (long long tb_i1301 = 0; tb_i1301 < tb_l1301->len; tb_i1301++) {
                tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1301->data)[tb_i1301];
                if (((tb_o_Node *)tb_nn(v_f, 1881))->q_is_cold) {
                    f_emit__put(v_c, ({ const char *tb_c1302_0 = "    "; long long tb_c1302_0n = sizeof("    ") - 1; const char *tb_c1302_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1882))->q_ty); long long tb_c1302_1n = (long long)strlen(tb_c1302_1); const char *tb_c1302_2 = " q_"; long long tb_c1302_2n = sizeof(" q_") - 1; const char *tb_c1302_3 = ((tb_o_Node *)tb_nn(v_f, 1882))->q_name; long long tb_c1302_3n = (long long)strlen(tb_c1302_3); const char *tb_c1302_4 = ";\n"; long long tb_c1302_4n = sizeof(";\n") - 1; char *tb_c1302r = tb_mem(tb_c1302_0n + tb_c1302_1n + tb_c1302_2n + tb_c1302_3n + tb_c1302_4n + 1), *tb_c1302rw = tb_c1302r; memcpy(tb_c1302rw, tb_c1302_0, tb_c1302_0n); tb_c1302rw += tb_c1302_0n; memcpy(tb_c1302rw, tb_c1302_1, tb_c1302_1n); tb_c1302rw += tb_c1302_1n; memcpy(tb_c1302rw, tb_c1302_2, tb_c1302_2n); tb_c1302rw += tb_c1302_2n; memcpy(tb_c1302rw, tb_c1302_3, tb_c1302_3n); tb_c1302rw += tb_c1302_3n; memcpy(tb_c1302rw, tb_c1302_4, tb_c1302_4n); tb_c1302rw += tb_c1302_4n; *tb_c1302rw = 0; (const char *)tb_c1302r; }));
                }
            }
        }
        f_emit__put(v_c, "};\n");
    }
    f_emit__put(v_c, ({ const char *tb_c1303_0 = "struct tb_o_"; long long tb_c1303_0n = sizeof("struct tb_o_") - 1; const char *tb_c1303_1 = ((tb_o_Node *)tb_nn(v_o, 1884))->q_name; long long tb_c1303_1n = (long long)strlen(tb_c1303_1); const char *tb_c1303_2 = " {\n"; long long tb_c1303_2n = sizeof(" {\n") - 1; char *tb_c1303r = tb_mem(tb_c1303_0n + tb_c1303_1n + tb_c1303_2n + 1), *tb_c1303rw = tb_c1303r; memcpy(tb_c1303rw, tb_c1303_0, tb_c1303_0n); tb_c1303rw += tb_c1303_0n; memcpy(tb_c1303rw, tb_c1303_1, tb_c1303_1n); tb_c1303rw += tb_c1303_1n; memcpy(tb_c1303rw, tb_c1303_2, tb_c1303_2n); tb_c1303rw += tb_c1303_2n; *tb_c1303rw = 0; (const char *)tb_c1303r; }));
    {
        tb_list * tb_l1304 = ((tb_o_Node *)tb_nn(v_o, 1885))->q_kids;
        for (long long tb_i1304 = 0; tb_i1304 < tb_l1304->len; tb_i1304++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1304->data)[tb_i1304];
            if ((!((tb_o_Node *)tb_nn(v_f, 1886))->q_is_cold)) {
                f_emit__put(v_c, ({ const char *tb_c1305_0 = "    "; long long tb_c1305_0n = sizeof("    ") - 1; const char *tb_c1305_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1887))->q_ty); long long tb_c1305_1n = (long long)strlen(tb_c1305_1); const char *tb_c1305_2 = " q_"; long long tb_c1305_2n = sizeof(" q_") - 1; const char *tb_c1305_3 = ((tb_o_Node *)tb_nn(v_f, 1887))->q_name; long long tb_c1305_3n = (long long)strlen(tb_c1305_3); const char *tb_c1305_4 = ";\n"; long long tb_c1305_4n = sizeof(";\n") - 1; char *tb_c1305r = tb_mem(tb_c1305_0n + tb_c1305_1n + tb_c1305_2n + tb_c1305_3n + tb_c1305_4n + 1), *tb_c1305rw = tb_c1305r; memcpy(tb_c1305rw, tb_c1305_0, tb_c1305_0n); tb_c1305rw += tb_c1305_0n; memcpy(tb_c1305rw, tb_c1305_1, tb_c1305_1n); tb_c1305rw += tb_c1305_1n; memcpy(tb_c1305rw, tb_c1305_2, tb_c1305_2n); tb_c1305rw += tb_c1305_2n; memcpy(tb_c1305rw, tb_c1305_3, tb_c1305_3n); tb_c1305rw += tb_c1305_3n; memcpy(tb_c1305rw, tb_c1305_4, tb_c1305_4n); tb_c1305rw += tb_c1305_4n; *tb_c1305rw = 0; (const char *)tb_c1305r; }));
            }
        }
    }
    if (f_emit__has_cold(v_o)) {
        f_emit__put(v_c, ({ const char *tb_c1306_0 = "    struct tb_c_"; long long tb_c1306_0n = sizeof("    struct tb_c_") - 1; const char *tb_c1306_1 = ((tb_o_Node *)tb_nn(v_o, 1889))->q_name; long long tb_c1306_1n = (long long)strlen(tb_c1306_1); const char *tb_c1306_2 = " *tb_cold;\n"; long long tb_c1306_2n = sizeof(" *tb_cold;\n") - 1; char *tb_c1306r = tb_mem(tb_c1306_0n + tb_c1306_1n + tb_c1306_2n + 1), *tb_c1306rw = tb_c1306r; memcpy(tb_c1306rw, tb_c1306_0, tb_c1306_0n); tb_c1306rw += tb_c1306_0n; memcpy(tb_c1306rw, tb_c1306_1, tb_c1306_1n); tb_c1306rw += tb_c1306_1n; memcpy(tb_c1306rw, tb_c1306_2, tb_c1306_2n); tb_c1306rw += tb_c1306_2n; *tb_c1306rw = 0; (const char *)tb_c1306r; }));
    }
    if (((((tb_o_Node *)tb_nn(v_o, 1890))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "    char empty;\n");
    }
    f_emit__put(v_c, "};\n");
}

static _Bool f_emit__has_cold(tb_o_Node * v_o) {
    {
        tb_list * tb_l1307 = ((tb_o_Node *)tb_nn(v_o, 1895))->q_kids;
        for (long long tb_i1307 = 0; tb_i1307 < tb_l1307->len; tb_i1307++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1307->data)[tb_i1307];
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
        tb_list * tb_l1308 = ((tb_o_Node *)tb_nn(v_o, 1902))->q_kids;
        for (long long tb_i1308 = 0; tb_i1308 < tb_l1308->len; tb_i1308++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1308->data)[tb_i1308];
            if (((strcmp(((tb_o_Node *)tb_nn(v_f, 1903))->q_name, v_name) == 0) && ((tb_o_Node *)tb_nn(v_f, 1903))->q_is_cold)) {
                return ({ const char *tb_c1309_0 = "->tb_cold->q_"; long long tb_c1309_0n = sizeof("->tb_cold->q_") - 1; const char *tb_c1309_1 = v_name; long long tb_c1309_1n = (long long)strlen(tb_c1309_1); char *tb_c1309r = tb_mem(tb_c1309_0n + tb_c1309_1n + 1), *tb_c1309rw = tb_c1309r; memcpy(tb_c1309rw, tb_c1309_0, tb_c1309_0n); tb_c1309rw += tb_c1309_0n; memcpy(tb_c1309rw, tb_c1309_1, tb_c1309_1n); tb_c1309rw += tb_c1309_1n; *tb_c1309rw = 0; (const char *)tb_c1309r; });
            }
        }
    }
    return ({ const char *tb_c1310_0 = "->q_"; long long tb_c1310_0n = sizeof("->q_") - 1; const char *tb_c1310_1 = v_name; long long tb_c1310_1n = (long long)strlen(tb_c1310_1); char *tb_c1310r = tb_mem(tb_c1310_0n + tb_c1310_1n + 1), *tb_c1310rw = tb_c1310r; memcpy(tb_c1310rw, tb_c1310_0, tb_c1310_0n); tb_c1310rw += tb_c1310_0n; memcpy(tb_c1310rw, tb_c1310_1, tb_c1310_1n); tb_c1310rw += tb_c1310_1n; *tb_c1310rw = 0; (const char *)tb_c1310r; });
    return v_r;
}

static void f_emit__emit_cols_helpers(tb_o_Comp * v_c, tb_o_Node * v_o) {
    const char * v_nm = ((tb_o_Node *)tb_nn(v_o, 1909))->q_name;
    const char * v_cols = ({ const char *tb_c1311_0 = "tb_cols_"; long long tb_c1311_0n = sizeof("tb_cols_") - 1; const char *tb_c1311_1 = v_nm; long long tb_c1311_1n = (long long)strlen(tb_c1311_1); char *tb_c1311r = tb_mem(tb_c1311_0n + tb_c1311_1n + 1), *tb_c1311rw = tb_c1311r; memcpy(tb_c1311rw, tb_c1311_0, tb_c1311_0n); tb_c1311rw += tb_c1311_0n; memcpy(tb_c1311rw, tb_c1311_1, tb_c1311_1n); tb_c1311rw += tb_c1311_1n; *tb_c1311rw = 0; (const char *)tb_c1311r; });
    const char * v_row = ({ const char *tb_c1312_0 = "tb_row_"; long long tb_c1312_0n = sizeof("tb_row_") - 1; const char *tb_c1312_1 = v_nm; long long tb_c1312_1n = (long long)strlen(tb_c1312_1); char *tb_c1312r = tb_mem(tb_c1312_0n + tb_c1312_1n + 1), *tb_c1312rw = tb_c1312r; memcpy(tb_c1312rw, tb_c1312_0, tb_c1312_0n); tb_c1312rw += tb_c1312_0n; memcpy(tb_c1312rw, tb_c1312_1, tb_c1312_1n); tb_c1312rw += tb_c1312_1n; *tb_c1312rw = 0; (const char *)tb_c1312r; });
    f_emit__put(v_c, ({ const char *tb_c1313_0 = "static "; long long tb_c1313_0n = sizeof("static ") - 1; const char *tb_c1313_1 = v_cols; long long tb_c1313_1n = (long long)strlen(tb_c1313_1); const char *tb_c1313_2 = " *tb_cols_new_"; long long tb_c1313_2n = sizeof(" *tb_cols_new_") - 1; const char *tb_c1313_3 = v_nm; long long tb_c1313_3n = (long long)strlen(tb_c1313_3); const char *tb_c1313_4 = "(void) {\n"; long long tb_c1313_4n = sizeof("(void) {\n") - 1; char *tb_c1313r = tb_mem(tb_c1313_0n + tb_c1313_1n + tb_c1313_2n + tb_c1313_3n + tb_c1313_4n + 1), *tb_c1313rw = tb_c1313r; memcpy(tb_c1313rw, tb_c1313_0, tb_c1313_0n); tb_c1313rw += tb_c1313_0n; memcpy(tb_c1313rw, tb_c1313_1, tb_c1313_1n); tb_c1313rw += tb_c1313_1n; memcpy(tb_c1313rw, tb_c1313_2, tb_c1313_2n); tb_c1313rw += tb_c1313_2n; memcpy(tb_c1313rw, tb_c1313_3, tb_c1313_3n); tb_c1313rw += tb_c1313_3n; memcpy(tb_c1313rw, tb_c1313_4, tb_c1313_4n); tb_c1313rw += tb_c1313_4n; *tb_c1313rw = 0; (const char *)tb_c1313r; }));
    f_emit__put(v_c, ({ const char *tb_c1314_0 = "    "; long long tb_c1314_0n = sizeof("    ") - 1; const char *tb_c1314_1 = v_cols; long long tb_c1314_1n = (long long)strlen(tb_c1314_1); const char *tb_c1314_2 = " *l = tb_mem(sizeof("; long long tb_c1314_2n = sizeof(" *l = tb_mem(sizeof(") - 1; const char *tb_c1314_3 = v_cols; long long tb_c1314_3n = (long long)strlen(tb_c1314_3); const char *tb_c1314_4 = "));\n    l->len = 0; l->cap = 4;\n"; long long tb_c1314_4n = sizeof("));\n    l->len = 0; l->cap = 4;\n") - 1; char *tb_c1314r = tb_mem(tb_c1314_0n + tb_c1314_1n + tb_c1314_2n + tb_c1314_3n + tb_c1314_4n + 1), *tb_c1314rw = tb_c1314r; memcpy(tb_c1314rw, tb_c1314_0, tb_c1314_0n); tb_c1314rw += tb_c1314_0n; memcpy(tb_c1314rw, tb_c1314_1, tb_c1314_1n); tb_c1314rw += tb_c1314_1n; memcpy(tb_c1314rw, tb_c1314_2, tb_c1314_2n); tb_c1314rw += tb_c1314_2n; memcpy(tb_c1314rw, tb_c1314_3, tb_c1314_3n); tb_c1314rw += tb_c1314_3n; memcpy(tb_c1314rw, tb_c1314_4, tb_c1314_4n); tb_c1314rw += tb_c1314_4n; *tb_c1314rw = 0; (const char *)tb_c1314r; }));
    {
        tb_list * tb_l1315 = ((tb_o_Node *)tb_nn(v_o, 1914))->q_kids;
        for (long long tb_i1315 = 0; tb_i1315 < tb_l1315->len; tb_i1315++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1315->data)[tb_i1315];
            f_emit__put(v_c, ({ const char *tb_c1316_0 = "    l->q_"; long long tb_c1316_0n = sizeof("    l->q_") - 1; const char *tb_c1316_1 = ((tb_o_Node *)tb_nn(v_f, 1915))->q_name; long long tb_c1316_1n = (long long)strlen(tb_c1316_1); const char *tb_c1316_2 = " = tb_mem(sizeof("; long long tb_c1316_2n = sizeof(" = tb_mem(sizeof(") - 1; const char *tb_c1316_3 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1915))->q_ty); long long tb_c1316_3n = (long long)strlen(tb_c1316_3); const char *tb_c1316_4 = ") * 4);\n"; long long tb_c1316_4n = sizeof(") * 4);\n") - 1; char *tb_c1316r = tb_mem(tb_c1316_0n + tb_c1316_1n + tb_c1316_2n + tb_c1316_3n + tb_c1316_4n + 1), *tb_c1316rw = tb_c1316r; memcpy(tb_c1316rw, tb_c1316_0, tb_c1316_0n); tb_c1316rw += tb_c1316_0n; memcpy(tb_c1316rw, tb_c1316_1, tb_c1316_1n); tb_c1316rw += tb_c1316_1n; memcpy(tb_c1316rw, tb_c1316_2, tb_c1316_2n); tb_c1316rw += tb_c1316_2n; memcpy(tb_c1316rw, tb_c1316_3, tb_c1316_3n); tb_c1316rw += tb_c1316_3n; memcpy(tb_c1316rw, tb_c1316_4, tb_c1316_4n); tb_c1316rw += tb_c1316_4n; *tb_c1316rw = 0; (const char *)tb_c1316r; }));
        }
    }
    f_emit__put(v_c, "    return l;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1317_0 = "static void tb_colgrow_"; long long tb_c1317_0n = sizeof("static void tb_colgrow_") - 1; const char *tb_c1317_1 = v_nm; long long tb_c1317_1n = (long long)strlen(tb_c1317_1); const char *tb_c1317_2 = "("; long long tb_c1317_2n = sizeof("(") - 1; const char *tb_c1317_3 = v_cols; long long tb_c1317_3n = (long long)strlen(tb_c1317_3); const char *tb_c1317_4 = " *l) {\n"; long long tb_c1317_4n = sizeof(" *l) {\n") - 1; char *tb_c1317r = tb_mem(tb_c1317_0n + tb_c1317_1n + tb_c1317_2n + tb_c1317_3n + tb_c1317_4n + 1), *tb_c1317rw = tb_c1317r; memcpy(tb_c1317rw, tb_c1317_0, tb_c1317_0n); tb_c1317rw += tb_c1317_0n; memcpy(tb_c1317rw, tb_c1317_1, tb_c1317_1n); tb_c1317rw += tb_c1317_1n; memcpy(tb_c1317rw, tb_c1317_2, tb_c1317_2n); tb_c1317rw += tb_c1317_2n; memcpy(tb_c1317rw, tb_c1317_3, tb_c1317_3n); tb_c1317rw += tb_c1317_3n; memcpy(tb_c1317rw, tb_c1317_4, tb_c1317_4n); tb_c1317rw += tb_c1317_4n; *tb_c1317rw = 0; (const char *)tb_c1317r; }));
    f_emit__put(v_c, "    if (l->len < l->cap) return;\n    l->cap *= 2;\n");
    {
        tb_list * tb_l1318 = ((tb_o_Node *)tb_nn(v_o, 1919))->q_kids;
        for (long long tb_i1318 = 0; tb_i1318 < tb_l1318->len; tb_i1318++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1318->data)[tb_i1318];
            const char * v_ft = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1920))->q_ty);
            f_emit__put(v_c, ({ const char *tb_c1319_0 = "    l->q_"; long long tb_c1319_0n = sizeof("    l->q_") - 1; const char *tb_c1319_1 = ((tb_o_Node *)tb_nn(v_f, 1921))->q_name; long long tb_c1319_1n = (long long)strlen(tb_c1319_1); const char *tb_c1319_2 = " = tb_resize(l->q_"; long long tb_c1319_2n = sizeof(" = tb_resize(l->q_") - 1; const char *tb_c1319_3 = ((tb_o_Node *)tb_nn(v_f, 1921))->q_name; long long tb_c1319_3n = (long long)strlen(tb_c1319_3); const char *tb_c1319_4 = ", sizeof("; long long tb_c1319_4n = sizeof(", sizeof(") - 1; const char *tb_c1319_5 = v_ft; long long tb_c1319_5n = (long long)strlen(tb_c1319_5); const char *tb_c1319_6 = ") * (l->cap / 2), sizeof("; long long tb_c1319_6n = sizeof(") * (l->cap / 2), sizeof(") - 1; const char *tb_c1319_7 = v_ft; long long tb_c1319_7n = (long long)strlen(tb_c1319_7); const char *tb_c1319_8 = ") * l->cap);\n"; long long tb_c1319_8n = sizeof(") * l->cap);\n") - 1; char *tb_c1319r = tb_mem(tb_c1319_0n + tb_c1319_1n + tb_c1319_2n + tb_c1319_3n + tb_c1319_4n + tb_c1319_5n + tb_c1319_6n + tb_c1319_7n + tb_c1319_8n + 1), *tb_c1319rw = tb_c1319r; memcpy(tb_c1319rw, tb_c1319_0, tb_c1319_0n); tb_c1319rw += tb_c1319_0n; memcpy(tb_c1319rw, tb_c1319_1, tb_c1319_1n); tb_c1319rw += tb_c1319_1n; memcpy(tb_c1319rw, tb_c1319_2, tb_c1319_2n); tb_c1319rw += tb_c1319_2n; memcpy(tb_c1319rw, tb_c1319_3, tb_c1319_3n); tb_c1319rw += tb_c1319_3n; memcpy(tb_c1319rw, tb_c1319_4, tb_c1319_4n); tb_c1319rw += tb_c1319_4n; memcpy(tb_c1319rw, tb_c1319_5, tb_c1319_5n); tb_c1319rw += tb_c1319_5n; memcpy(tb_c1319rw, tb_c1319_6, tb_c1319_6n); tb_c1319rw += tb_c1319_6n; memcpy(tb_c1319rw, tb_c1319_7, tb_c1319_7n); tb_c1319rw += tb_c1319_7n; memcpy(tb_c1319rw, tb_c1319_8, tb_c1319_8n); tb_c1319rw += tb_c1319_8n; *tb_c1319rw = 0; (const char *)tb_c1319r; }));
        }
    }
    f_emit__put(v_c, "}\n");
    f_emit__put(v_c, ({ const char *tb_c1320_0 = "static "; long long tb_c1320_0n = sizeof("static ") - 1; const char *tb_c1320_1 = v_row; long long tb_c1320_1n = (long long)strlen(tb_c1320_1); const char *tb_c1320_2 = " tb_colat_"; long long tb_c1320_2n = sizeof(" tb_colat_") - 1; const char *tb_c1320_3 = v_nm; long long tb_c1320_3n = (long long)strlen(tb_c1320_3); const char *tb_c1320_4 = "("; long long tb_c1320_4n = sizeof("(") - 1; const char *tb_c1320_5 = v_cols; long long tb_c1320_5n = (long long)strlen(tb_c1320_5); const char *tb_c1320_6 = " *l, long long i, int line) {\n"; long long tb_c1320_6n = sizeof(" *l, long long i, int line) {\n") - 1; char *tb_c1320r = tb_mem(tb_c1320_0n + tb_c1320_1n + tb_c1320_2n + tb_c1320_3n + tb_c1320_4n + tb_c1320_5n + tb_c1320_6n + 1), *tb_c1320rw = tb_c1320r; memcpy(tb_c1320rw, tb_c1320_0, tb_c1320_0n); tb_c1320rw += tb_c1320_0n; memcpy(tb_c1320rw, tb_c1320_1, tb_c1320_1n); tb_c1320rw += tb_c1320_1n; memcpy(tb_c1320rw, tb_c1320_2, tb_c1320_2n); tb_c1320rw += tb_c1320_2n; memcpy(tb_c1320rw, tb_c1320_3, tb_c1320_3n); tb_c1320rw += tb_c1320_3n; memcpy(tb_c1320rw, tb_c1320_4, tb_c1320_4n); tb_c1320rw += tb_c1320_4n; memcpy(tb_c1320rw, tb_c1320_5, tb_c1320_5n); tb_c1320rw += tb_c1320_5n; memcpy(tb_c1320rw, tb_c1320_6, tb_c1320_6n); tb_c1320rw += tb_c1320_6n; *tb_c1320rw = 0; (const char *)tb_c1320r; }));
    f_emit__put(v_c, ({ const char *tb_c1321_0 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)(("; long long tb_c1321_0n = sizeof("    if (TB_RARE((unsigned long long)i >= (unsigned long long)((") - 1; const char *tb_c1321_1 = v_cols; long long tb_c1321_1n = (long long)strlen(tb_c1321_1); const char *tb_c1321_2 = " *)tb_nn(l, line))->len)) tb_crash(line, \"номер за пределами списка\");\n"; long long tb_c1321_2n = sizeof(" *)tb_nn(l, line))->len)) tb_crash(line, \"номер за пределами списка\");\n") - 1; char *tb_c1321r = tb_mem(tb_c1321_0n + tb_c1321_1n + tb_c1321_2n + 1), *tb_c1321rw = tb_c1321r; memcpy(tb_c1321rw, tb_c1321_0, tb_c1321_0n); tb_c1321rw += tb_c1321_0n; memcpy(tb_c1321rw, tb_c1321_1, tb_c1321_1n); tb_c1321rw += tb_c1321_1n; memcpy(tb_c1321rw, tb_c1321_2, tb_c1321_2n); tb_c1321rw += tb_c1321_2n; *tb_c1321rw = 0; (const char *)tb_c1321r; }));
    f_emit__put(v_c, ({ const char *tb_c1322_0 = "    return ("; long long tb_c1322_0n = sizeof("    return (") - 1; const char *tb_c1322_1 = v_row; long long tb_c1322_1n = (long long)strlen(tb_c1322_1); const char *tb_c1322_2 = "){l, i};\n}\n"; long long tb_c1322_2n = sizeof("){l, i};\n}\n") - 1; char *tb_c1322r = tb_mem(tb_c1322_0n + tb_c1322_1n + tb_c1322_2n + 1), *tb_c1322rw = tb_c1322r; memcpy(tb_c1322rw, tb_c1322_0, tb_c1322_0n); tb_c1322rw += tb_c1322_0n; memcpy(tb_c1322rw, tb_c1322_1, tb_c1322_1n); tb_c1322rw += tb_c1322_1n; memcpy(tb_c1322rw, tb_c1322_2, tb_c1322_2n); tb_c1322rw += tb_c1322_2n; *tb_c1322rw = 0; (const char *)tb_c1322r; }));
    {
        tb_list * tb_l1323 = ((tb_o_Node *)tb_nn(v_o, 1926))->q_kids;
        for (long long tb_i1323 = 0; tb_i1323 < tb_l1323->len; tb_i1323++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1323->data)[tb_i1323];
            const char * v_ft = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn(v_f, 1927))->q_ty);
            f_emit__put(v_c, ({ const char *tb_c1324_0 = "static inline "; long long tb_c1324_0n = sizeof("static inline ") - 1; const char *tb_c1324_1 = v_ft; long long tb_c1324_1n = (long long)strlen(tb_c1324_1); const char *tb_c1324_2 = " *tb_colf_"; long long tb_c1324_2n = sizeof(" *tb_colf_") - 1; const char *tb_c1324_3 = v_nm; long long tb_c1324_3n = (long long)strlen(tb_c1324_3); const char *tb_c1324_4 = "_"; long long tb_c1324_4n = sizeof("_") - 1; const char *tb_c1324_5 = ((tb_o_Node *)tb_nn(v_f, 1928))->q_name; long long tb_c1324_5n = (long long)strlen(tb_c1324_5); const char *tb_c1324_6 = "("; long long tb_c1324_6n = sizeof("(") - 1; const char *tb_c1324_7 = v_row; long long tb_c1324_7n = (long long)strlen(tb_c1324_7); const char *tb_c1324_8 = " r, int line) {\n"; long long tb_c1324_8n = sizeof(" r, int line) {\n") - 1; char *tb_c1324r = tb_mem(tb_c1324_0n + tb_c1324_1n + tb_c1324_2n + tb_c1324_3n + tb_c1324_4n + tb_c1324_5n + tb_c1324_6n + tb_c1324_7n + tb_c1324_8n + 1), *tb_c1324rw = tb_c1324r; memcpy(tb_c1324rw, tb_c1324_0, tb_c1324_0n); tb_c1324rw += tb_c1324_0n; memcpy(tb_c1324rw, tb_c1324_1, tb_c1324_1n); tb_c1324rw += tb_c1324_1n; memcpy(tb_c1324rw, tb_c1324_2, tb_c1324_2n); tb_c1324rw += tb_c1324_2n; memcpy(tb_c1324rw, tb_c1324_3, tb_c1324_3n); tb_c1324rw += tb_c1324_3n; memcpy(tb_c1324rw, tb_c1324_4, tb_c1324_4n); tb_c1324rw += tb_c1324_4n; memcpy(tb_c1324rw, tb_c1324_5, tb_c1324_5n); tb_c1324rw += tb_c1324_5n; memcpy(tb_c1324rw, tb_c1324_6, tb_c1324_6n); tb_c1324rw += tb_c1324_6n; memcpy(tb_c1324rw, tb_c1324_7, tb_c1324_7n); tb_c1324rw += tb_c1324_7n; memcpy(tb_c1324rw, tb_c1324_8, tb_c1324_8n); tb_c1324rw += tb_c1324_8n; *tb_c1324rw = 0; (const char *)tb_c1324r; }));
            f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
            f_emit__put(v_c, ({ const char *tb_c1325_0 = "    return &r.c->q_"; long long tb_c1325_0n = sizeof("    return &r.c->q_") - 1; const char *tb_c1325_1 = ((tb_o_Node *)tb_nn(v_f, 1930))->q_name; long long tb_c1325_1n = (long long)strlen(tb_c1325_1); const char *tb_c1325_2 = "[r.i];\n}\n"; long long tb_c1325_2n = sizeof("[r.i];\n}\n") - 1; char *tb_c1325r = tb_mem(tb_c1325_0n + tb_c1325_1n + tb_c1325_2n + 1), *tb_c1325rw = tb_c1325r; memcpy(tb_c1325rw, tb_c1325_0, tb_c1325_0n); tb_c1325rw += tb_c1325_0n; memcpy(tb_c1325rw, tb_c1325_1, tb_c1325_1n); tb_c1325rw += tb_c1325_1n; memcpy(tb_c1325rw, tb_c1325_2, tb_c1325_2n); tb_c1325rw += tb_c1325_2n; *tb_c1325rw = 0; (const char *)tb_c1325r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1326_0 = "static void tb_coladd_"; long long tb_c1326_0n = sizeof("static void tb_coladd_") - 1; const char *tb_c1326_1 = v_nm; long long tb_c1326_1n = (long long)strlen(tb_c1326_1); const char *tb_c1326_2 = "("; long long tb_c1326_2n = sizeof("(") - 1; const char *tb_c1326_3 = v_cols; long long tb_c1326_3n = (long long)strlen(tb_c1326_3); const char *tb_c1326_4 = " *l, "; long long tb_c1326_4n = sizeof(" *l, ") - 1; const char *tb_c1326_5 = v_row; long long tb_c1326_5n = (long long)strlen(tb_c1326_5); const char *tb_c1326_6 = " r, int line) {\n"; long long tb_c1326_6n = sizeof(" r, int line) {\n") - 1; char *tb_c1326r = tb_mem(tb_c1326_0n + tb_c1326_1n + tb_c1326_2n + tb_c1326_3n + tb_c1326_4n + tb_c1326_5n + tb_c1326_6n + 1), *tb_c1326rw = tb_c1326r; memcpy(tb_c1326rw, tb_c1326_0, tb_c1326_0n); tb_c1326rw += tb_c1326_0n; memcpy(tb_c1326rw, tb_c1326_1, tb_c1326_1n); tb_c1326rw += tb_c1326_1n; memcpy(tb_c1326rw, tb_c1326_2, tb_c1326_2n); tb_c1326rw += tb_c1326_2n; memcpy(tb_c1326rw, tb_c1326_3, tb_c1326_3n); tb_c1326rw += tb_c1326_3n; memcpy(tb_c1326rw, tb_c1326_4, tb_c1326_4n); tb_c1326rw += tb_c1326_4n; memcpy(tb_c1326rw, tb_c1326_5, tb_c1326_5n); tb_c1326rw += tb_c1326_5n; memcpy(tb_c1326rw, tb_c1326_6, tb_c1326_6n); tb_c1326rw += tb_c1326_6n; *tb_c1326rw = 0; (const char *)tb_c1326r; }));
    f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
    {
        long long tb_end1327 = ((((tb_o_Node *)tb_nn(v_o, 1933))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1327; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1328_0 = "    "; long long tb_c1328_0n = sizeof("    ") - 1; const char *tb_c1328_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1934))->q_kids, v_i, sizeof(tb_o_Node *), 1934)), 1934))->q_ty); long long tb_c1328_1n = (long long)strlen(tb_c1328_1); const char *tb_c1328_2 = " a"; long long tb_c1328_2n = sizeof(" a") - 1; char tb_c1328_3d[24]; long long tb_c1328_3n; const char *tb_c1328_3 = tb_digits(v_i, tb_c1328_3d, &tb_c1328_3n); const char *tb_c1328_4 = " = r.c->q_"; long long tb_c1328_4n = sizeof(" = r.c->q_") - 1; const char *tb_c1328_5 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1934))->q_kids, v_i, sizeof(tb_o_Node *), 1934)), 1934))->q_name; long long tb_c1328_5n = (long long)strlen(tb_c1328_5); const char *tb_c1328_6 = "[r.i];\n"; long long tb_c1328_6n = sizeof("[r.i];\n") - 1; char *tb_c1328r = tb_mem(tb_c1328_0n + tb_c1328_1n + tb_c1328_2n + tb_c1328_3n + tb_c1328_4n + tb_c1328_5n + tb_c1328_6n + 1), *tb_c1328rw = tb_c1328r; memcpy(tb_c1328rw, tb_c1328_0, tb_c1328_0n); tb_c1328rw += tb_c1328_0n; memcpy(tb_c1328rw, tb_c1328_1, tb_c1328_1n); tb_c1328rw += tb_c1328_1n; memcpy(tb_c1328rw, tb_c1328_2, tb_c1328_2n); tb_c1328rw += tb_c1328_2n; memcpy(tb_c1328rw, tb_c1328_3, tb_c1328_3n); tb_c1328rw += tb_c1328_3n; memcpy(tb_c1328rw, tb_c1328_4, tb_c1328_4n); tb_c1328rw += tb_c1328_4n; memcpy(tb_c1328rw, tb_c1328_5, tb_c1328_5n); tb_c1328rw += tb_c1328_5n; memcpy(tb_c1328rw, tb_c1328_6, tb_c1328_6n); tb_c1328rw += tb_c1328_6n; *tb_c1328rw = 0; (const char *)tb_c1328r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1329_0 = "    tb_colgrow_"; long long tb_c1329_0n = sizeof("    tb_colgrow_") - 1; const char *tb_c1329_1 = v_nm; long long tb_c1329_1n = (long long)strlen(tb_c1329_1); const char *tb_c1329_2 = "(tb_nn(l, line));\n"; long long tb_c1329_2n = sizeof("(tb_nn(l, line));\n") - 1; char *tb_c1329r = tb_mem(tb_c1329_0n + tb_c1329_1n + tb_c1329_2n + 1), *tb_c1329rw = tb_c1329r; memcpy(tb_c1329rw, tb_c1329_0, tb_c1329_0n); tb_c1329rw += tb_c1329_0n; memcpy(tb_c1329rw, tb_c1329_1, tb_c1329_1n); tb_c1329rw += tb_c1329_1n; memcpy(tb_c1329rw, tb_c1329_2, tb_c1329_2n); tb_c1329rw += tb_c1329_2n; *tb_c1329rw = 0; (const char *)tb_c1329r; }));
    {
        long long tb_end1330 = ((((tb_o_Node *)tb_nn(v_o, 1936))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1330; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1331_0 = "    l->q_"; long long tb_c1331_0n = sizeof("    l->q_") - 1; const char *tb_c1331_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1937))->q_kids, v_i, sizeof(tb_o_Node *), 1937)), 1937))->q_name; long long tb_c1331_1n = (long long)strlen(tb_c1331_1); const char *tb_c1331_2 = "[l->len] = a"; long long tb_c1331_2n = sizeof("[l->len] = a") - 1; char tb_c1331_3d[24]; long long tb_c1331_3n; const char *tb_c1331_3 = tb_digits(v_i, tb_c1331_3d, &tb_c1331_3n); const char *tb_c1331_4 = ";\n"; long long tb_c1331_4n = sizeof(";\n") - 1; char *tb_c1331r = tb_mem(tb_c1331_0n + tb_c1331_1n + tb_c1331_2n + tb_c1331_3n + tb_c1331_4n + 1), *tb_c1331rw = tb_c1331r; memcpy(tb_c1331rw, tb_c1331_0, tb_c1331_0n); tb_c1331rw += tb_c1331_0n; memcpy(tb_c1331rw, tb_c1331_1, tb_c1331_1n); tb_c1331rw += tb_c1331_1n; memcpy(tb_c1331rw, tb_c1331_2, tb_c1331_2n); tb_c1331rw += tb_c1331_2n; memcpy(tb_c1331rw, tb_c1331_3, tb_c1331_3n); tb_c1331rw += tb_c1331_3n; memcpy(tb_c1331rw, tb_c1331_4, tb_c1331_4n); tb_c1331rw += tb_c1331_4n; *tb_c1331rw = 0; (const char *)tb_c1331r; }));
        }
    }
    f_emit__put(v_c, "    l->len += 1;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1332_0 = "static void tb_colpush_"; long long tb_c1332_0n = sizeof("static void tb_colpush_") - 1; const char *tb_c1332_1 = v_nm; long long tb_c1332_1n = (long long)strlen(tb_c1332_1); const char *tb_c1332_2 = "("; long long tb_c1332_2n = sizeof("(") - 1; const char *tb_c1332_3 = v_cols; long long tb_c1332_3n = (long long)strlen(tb_c1332_3); const char *tb_c1332_4 = " *l"; long long tb_c1332_4n = sizeof(" *l") - 1; char *tb_c1332r = tb_mem(tb_c1332_0n + tb_c1332_1n + tb_c1332_2n + tb_c1332_3n + tb_c1332_4n + 1), *tb_c1332rw = tb_c1332r; memcpy(tb_c1332rw, tb_c1332_0, tb_c1332_0n); tb_c1332rw += tb_c1332_0n; memcpy(tb_c1332rw, tb_c1332_1, tb_c1332_1n); tb_c1332rw += tb_c1332_1n; memcpy(tb_c1332rw, tb_c1332_2, tb_c1332_2n); tb_c1332rw += tb_c1332_2n; memcpy(tb_c1332rw, tb_c1332_3, tb_c1332_3n); tb_c1332rw += tb_c1332_3n; memcpy(tb_c1332rw, tb_c1332_4, tb_c1332_4n); tb_c1332rw += tb_c1332_4n; *tb_c1332rw = 0; (const char *)tb_c1332r; }));
    {
        long long tb_end1333 = ((((tb_o_Node *)tb_nn(v_o, 1940))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1333; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1334_0 = ", "; long long tb_c1334_0n = sizeof(", ") - 1; const char *tb_c1334_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1941))->q_kids, v_i, sizeof(tb_o_Node *), 1941)), 1941))->q_ty); long long tb_c1334_1n = (long long)strlen(tb_c1334_1); const char *tb_c1334_2 = " a"; long long tb_c1334_2n = sizeof(" a") - 1; char tb_c1334_3d[24]; long long tb_c1334_3n; const char *tb_c1334_3 = tb_digits(v_i, tb_c1334_3d, &tb_c1334_3n); char *tb_c1334r = tb_mem(tb_c1334_0n + tb_c1334_1n + tb_c1334_2n + tb_c1334_3n + 1), *tb_c1334rw = tb_c1334r; memcpy(tb_c1334rw, tb_c1334_0, tb_c1334_0n); tb_c1334rw += tb_c1334_0n; memcpy(tb_c1334rw, tb_c1334_1, tb_c1334_1n); tb_c1334rw += tb_c1334_1n; memcpy(tb_c1334rw, tb_c1334_2, tb_c1334_2n); tb_c1334rw += tb_c1334_2n; memcpy(tb_c1334rw, tb_c1334_3, tb_c1334_3n); tb_c1334rw += tb_c1334_3n; *tb_c1334rw = 0; (const char *)tb_c1334r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1335_0 = ", int line) {\n    tb_colgrow_"; long long tb_c1335_0n = sizeof(", int line) {\n    tb_colgrow_") - 1; const char *tb_c1335_1 = v_nm; long long tb_c1335_1n = (long long)strlen(tb_c1335_1); const char *tb_c1335_2 = "(tb_nn(l, line));\n"; long long tb_c1335_2n = sizeof("(tb_nn(l, line));\n") - 1; char *tb_c1335r = tb_mem(tb_c1335_0n + tb_c1335_1n + tb_c1335_2n + 1), *tb_c1335rw = tb_c1335r; memcpy(tb_c1335rw, tb_c1335_0, tb_c1335_0n); tb_c1335rw += tb_c1335_0n; memcpy(tb_c1335rw, tb_c1335_1, tb_c1335_1n); tb_c1335rw += tb_c1335_1n; memcpy(tb_c1335rw, tb_c1335_2, tb_c1335_2n); tb_c1335rw += tb_c1335_2n; *tb_c1335rw = 0; (const char *)tb_c1335r; }));
    {
        long long tb_end1336 = ((((tb_o_Node *)tb_nn(v_o, 1943))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1336; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1337_0 = "    l->q_"; long long tb_c1337_0n = sizeof("    l->q_") - 1; const char *tb_c1337_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1944))->q_kids, v_i, sizeof(tb_o_Node *), 1944)), 1944))->q_name; long long tb_c1337_1n = (long long)strlen(tb_c1337_1); const char *tb_c1337_2 = "[l->len] = a"; long long tb_c1337_2n = sizeof("[l->len] = a") - 1; char tb_c1337_3d[24]; long long tb_c1337_3n; const char *tb_c1337_3 = tb_digits(v_i, tb_c1337_3d, &tb_c1337_3n); const char *tb_c1337_4 = ";\n"; long long tb_c1337_4n = sizeof(";\n") - 1; char *tb_c1337r = tb_mem(tb_c1337_0n + tb_c1337_1n + tb_c1337_2n + tb_c1337_3n + tb_c1337_4n + 1), *tb_c1337rw = tb_c1337r; memcpy(tb_c1337rw, tb_c1337_0, tb_c1337_0n); tb_c1337rw += tb_c1337_0n; memcpy(tb_c1337rw, tb_c1337_1, tb_c1337_1n); tb_c1337rw += tb_c1337_1n; memcpy(tb_c1337rw, tb_c1337_2, tb_c1337_2n); tb_c1337rw += tb_c1337_2n; memcpy(tb_c1337rw, tb_c1337_3, tb_c1337_3n); tb_c1337rw += tb_c1337_3n; memcpy(tb_c1337rw, tb_c1337_4, tb_c1337_4n); tb_c1337rw += tb_c1337_4n; *tb_c1337rw = 0; (const char *)tb_c1337r; }));
        }
    }
    f_emit__put(v_c, "    l->len += 1;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1338_0 = "static void tb_colset_"; long long tb_c1338_0n = sizeof("static void tb_colset_") - 1; const char *tb_c1338_1 = v_nm; long long tb_c1338_1n = (long long)strlen(tb_c1338_1); const char *tb_c1338_2 = "("; long long tb_c1338_2n = sizeof("(") - 1; const char *tb_c1338_3 = v_cols; long long tb_c1338_3n = (long long)strlen(tb_c1338_3); const char *tb_c1338_4 = " *l, long long i, "; long long tb_c1338_4n = sizeof(" *l, long long i, ") - 1; const char *tb_c1338_5 = v_row; long long tb_c1338_5n = (long long)strlen(tb_c1338_5); const char *tb_c1338_6 = " r, int line) {\n"; long long tb_c1338_6n = sizeof(" r, int line) {\n") - 1; char *tb_c1338r = tb_mem(tb_c1338_0n + tb_c1338_1n + tb_c1338_2n + tb_c1338_3n + tb_c1338_4n + tb_c1338_5n + tb_c1338_6n + 1), *tb_c1338rw = tb_c1338r; memcpy(tb_c1338rw, tb_c1338_0, tb_c1338_0n); tb_c1338rw += tb_c1338_0n; memcpy(tb_c1338rw, tb_c1338_1, tb_c1338_1n); tb_c1338rw += tb_c1338_1n; memcpy(tb_c1338rw, tb_c1338_2, tb_c1338_2n); tb_c1338rw += tb_c1338_2n; memcpy(tb_c1338rw, tb_c1338_3, tb_c1338_3n); tb_c1338rw += tb_c1338_3n; memcpy(tb_c1338rw, tb_c1338_4, tb_c1338_4n); tb_c1338rw += tb_c1338_4n; memcpy(tb_c1338rw, tb_c1338_5, tb_c1338_5n); tb_c1338rw += tb_c1338_5n; memcpy(tb_c1338rw, tb_c1338_6, tb_c1338_6n); tb_c1338rw += tb_c1338_6n; *tb_c1338rw = 0; (const char *)tb_c1338r; }));
    f_emit__put(v_c, ({ const char *tb_c1339_0 = "    tb_colat_"; long long tb_c1339_0n = sizeof("    tb_colat_") - 1; const char *tb_c1339_1 = v_nm; long long tb_c1339_1n = (long long)strlen(tb_c1339_1); const char *tb_c1339_2 = "(l, i, line);\n"; long long tb_c1339_2n = sizeof("(l, i, line);\n") - 1; char *tb_c1339r = tb_mem(tb_c1339_0n + tb_c1339_1n + tb_c1339_2n + 1), *tb_c1339rw = tb_c1339r; memcpy(tb_c1339rw, tb_c1339_0, tb_c1339_0n); tb_c1339rw += tb_c1339_0n; memcpy(tb_c1339rw, tb_c1339_1, tb_c1339_1n); tb_c1339rw += tb_c1339_1n; memcpy(tb_c1339rw, tb_c1339_2, tb_c1339_2n); tb_c1339rw += tb_c1339_2n; *tb_c1339rw = 0; (const char *)tb_c1339r; }));
    f_emit__put(v_c, "    if (TB_RARE(r.c == 0)) tb_crash(line, \"объект пустой: его ещё не создали\");\n");
    {
        tb_list * tb_l1340 = ((tb_o_Node *)tb_nn(v_o, 1949))->q_kids;
        for (long long tb_i1340 = 0; tb_i1340 < tb_l1340->len; tb_i1340++) {
            tb_o_Node * const v_f = ((tb_o_Node * *)tb_l1340->data)[tb_i1340];
            f_emit__put(v_c, ({ const char *tb_c1341_0 = "    l->q_"; long long tb_c1341_0n = sizeof("    l->q_") - 1; const char *tb_c1341_1 = ((tb_o_Node *)tb_nn(v_f, 1950))->q_name; long long tb_c1341_1n = (long long)strlen(tb_c1341_1); const char *tb_c1341_2 = "[i] = r.c->q_"; long long tb_c1341_2n = sizeof("[i] = r.c->q_") - 1; const char *tb_c1341_3 = ((tb_o_Node *)tb_nn(v_f, 1950))->q_name; long long tb_c1341_3n = (long long)strlen(tb_c1341_3); const char *tb_c1341_4 = "[r.i];\n"; long long tb_c1341_4n = sizeof("[r.i];\n") - 1; char *tb_c1341r = tb_mem(tb_c1341_0n + tb_c1341_1n + tb_c1341_2n + tb_c1341_3n + tb_c1341_4n + 1), *tb_c1341rw = tb_c1341r; memcpy(tb_c1341rw, tb_c1341_0, tb_c1341_0n); tb_c1341rw += tb_c1341_0n; memcpy(tb_c1341rw, tb_c1341_1, tb_c1341_1n); tb_c1341rw += tb_c1341_1n; memcpy(tb_c1341rw, tb_c1341_2, tb_c1341_2n); tb_c1341rw += tb_c1341_2n; memcpy(tb_c1341rw, tb_c1341_3, tb_c1341_3n); tb_c1341rw += tb_c1341_3n; memcpy(tb_c1341rw, tb_c1341_4, tb_c1341_4n); tb_c1341rw += tb_c1341_4n; *tb_c1341rw = 0; (const char *)tb_c1341r; }));
        }
    }
    f_emit__put(v_c, "}\n");
    f_emit__put(v_c, ({ const char *tb_c1342_0 = "static "; long long tb_c1342_0n = sizeof("static ") - 1; const char *tb_c1342_1 = v_cols; long long tb_c1342_1n = (long long)strlen(tb_c1342_1); const char *tb_c1342_2 = " *tb_colsfrom_"; long long tb_c1342_2n = sizeof(" *tb_colsfrom_") - 1; const char *tb_c1342_3 = v_nm; long long tb_c1342_3n = (long long)strlen(tb_c1342_3); const char *tb_c1342_4 = "(long long n, "; long long tb_c1342_4n = sizeof("(long long n, ") - 1; const char *tb_c1342_5 = v_row; long long tb_c1342_5n = (long long)strlen(tb_c1342_5); const char *tb_c1342_6 = " *rows) {\n"; long long tb_c1342_6n = sizeof(" *rows) {\n") - 1; char *tb_c1342r = tb_mem(tb_c1342_0n + tb_c1342_1n + tb_c1342_2n + tb_c1342_3n + tb_c1342_4n + tb_c1342_5n + tb_c1342_6n + 1), *tb_c1342rw = tb_c1342r; memcpy(tb_c1342rw, tb_c1342_0, tb_c1342_0n); tb_c1342rw += tb_c1342_0n; memcpy(tb_c1342rw, tb_c1342_1, tb_c1342_1n); tb_c1342rw += tb_c1342_1n; memcpy(tb_c1342rw, tb_c1342_2, tb_c1342_2n); tb_c1342rw += tb_c1342_2n; memcpy(tb_c1342rw, tb_c1342_3, tb_c1342_3n); tb_c1342rw += tb_c1342_3n; memcpy(tb_c1342rw, tb_c1342_4, tb_c1342_4n); tb_c1342rw += tb_c1342_4n; memcpy(tb_c1342rw, tb_c1342_5, tb_c1342_5n); tb_c1342rw += tb_c1342_5n; memcpy(tb_c1342rw, tb_c1342_6, tb_c1342_6n); tb_c1342rw += tb_c1342_6n; *tb_c1342rw = 0; (const char *)tb_c1342r; }));
    f_emit__put(v_c, ({ const char *tb_c1343_0 = "    "; long long tb_c1343_0n = sizeof("    ") - 1; const char *tb_c1343_1 = v_cols; long long tb_c1343_1n = (long long)strlen(tb_c1343_1); const char *tb_c1343_2 = " *l = tb_cols_new_"; long long tb_c1343_2n = sizeof(" *l = tb_cols_new_") - 1; const char *tb_c1343_3 = v_nm; long long tb_c1343_3n = (long long)strlen(tb_c1343_3); const char *tb_c1343_4 = "();\n"; long long tb_c1343_4n = sizeof("();\n") - 1; char *tb_c1343r = tb_mem(tb_c1343_0n + tb_c1343_1n + tb_c1343_2n + tb_c1343_3n + tb_c1343_4n + 1), *tb_c1343rw = tb_c1343r; memcpy(tb_c1343rw, tb_c1343_0, tb_c1343_0n); tb_c1343rw += tb_c1343_0n; memcpy(tb_c1343rw, tb_c1343_1, tb_c1343_1n); tb_c1343rw += tb_c1343_1n; memcpy(tb_c1343rw, tb_c1343_2, tb_c1343_2n); tb_c1343rw += tb_c1343_2n; memcpy(tb_c1343rw, tb_c1343_3, tb_c1343_3n); tb_c1343rw += tb_c1343_3n; memcpy(tb_c1343rw, tb_c1343_4, tb_c1343_4n); tb_c1343rw += tb_c1343_4n; *tb_c1343rw = 0; (const char *)tb_c1343r; }));
    f_emit__put(v_c, ({ const char *tb_c1344_0 = "    for (long long i = 0; i < n; i++) tb_coladd_"; long long tb_c1344_0n = sizeof("    for (long long i = 0; i < n; i++) tb_coladd_") - 1; const char *tb_c1344_1 = v_nm; long long tb_c1344_1n = (long long)strlen(tb_c1344_1); const char *tb_c1344_2 = "(l, rows[i], 0);\n"; long long tb_c1344_2n = sizeof("(l, rows[i], 0);\n") - 1; char *tb_c1344r = tb_mem(tb_c1344_0n + tb_c1344_1n + tb_c1344_2n + 1), *tb_c1344rw = tb_c1344r; memcpy(tb_c1344rw, tb_c1344_0, tb_c1344_0n); tb_c1344rw += tb_c1344_0n; memcpy(tb_c1344rw, tb_c1344_1, tb_c1344_1n); tb_c1344rw += tb_c1344_1n; memcpy(tb_c1344rw, tb_c1344_2, tb_c1344_2n); tb_c1344rw += tb_c1344_2n; *tb_c1344rw = 0; (const char *)tb_c1344r; }));
    f_emit__put(v_c, "    return l;\n}\n");
    f_emit__put(v_c, ({ const char *tb_c1345_0 = "static "; long long tb_c1345_0n = sizeof("static ") - 1; const char *tb_c1345_1 = v_row; long long tb_c1345_1n = (long long)strlen(tb_c1345_1); const char *tb_c1345_2 = " tb_new_"; long long tb_c1345_2n = sizeof(" tb_new_") - 1; const char *tb_c1345_3 = v_nm; long long tb_c1345_3n = (long long)strlen(tb_c1345_3); const char *tb_c1345_4 = "("; long long tb_c1345_4n = sizeof("(") - 1; char *tb_c1345r = tb_mem(tb_c1345_0n + tb_c1345_1n + tb_c1345_2n + tb_c1345_3n + tb_c1345_4n + 1), *tb_c1345rw = tb_c1345r; memcpy(tb_c1345rw, tb_c1345_0, tb_c1345_0n); tb_c1345rw += tb_c1345_0n; memcpy(tb_c1345rw, tb_c1345_1, tb_c1345_1n); tb_c1345rw += tb_c1345_1n; memcpy(tb_c1345rw, tb_c1345_2, tb_c1345_2n); tb_c1345rw += tb_c1345_2n; memcpy(tb_c1345rw, tb_c1345_3, tb_c1345_3n); tb_c1345rw += tb_c1345_3n; memcpy(tb_c1345rw, tb_c1345_4, tb_c1345_4n); tb_c1345rw += tb_c1345_4n; *tb_c1345rw = 0; (const char *)tb_c1345r; }));
    if (((((tb_o_Node *)tb_nn(v_o, 1958))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end1346 = ((((tb_o_Node *)tb_nn(v_o, 1960))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1346; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1347_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1963))->q_kids, v_i, sizeof(tb_o_Node *), 1963)), 1963))->q_ty); long long tb_c1347_0n = (long long)strlen(tb_c1347_0); const char *tb_c1347_1 = " a"; long long tb_c1347_1n = sizeof(" a") - 1; char tb_c1347_2d[24]; long long tb_c1347_2n; const char *tb_c1347_2 = tb_digits(v_i, tb_c1347_2d, &tb_c1347_2n); char *tb_c1347r = tb_mem(tb_c1347_0n + tb_c1347_1n + tb_c1347_2n + 1), *tb_c1347rw = tb_c1347r; memcpy(tb_c1347rw, tb_c1347_0, tb_c1347_0n); tb_c1347rw += tb_c1347_0n; memcpy(tb_c1347rw, tb_c1347_1, tb_c1347_1n); tb_c1347rw += tb_c1347_1n; memcpy(tb_c1347rw, tb_c1347_2, tb_c1347_2n); tb_c1347rw += tb_c1347_2n; *tb_c1347rw = 0; (const char *)tb_c1347r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1348_0 = ") {\n    "; long long tb_c1348_0n = sizeof(") {\n    ") - 1; const char *tb_c1348_1 = v_cols; long long tb_c1348_1n = (long long)strlen(tb_c1348_1); const char *tb_c1348_2 = " *l = tb_cols_new_"; long long tb_c1348_2n = sizeof(" *l = tb_cols_new_") - 1; const char *tb_c1348_3 = v_nm; long long tb_c1348_3n = (long long)strlen(tb_c1348_3); const char *tb_c1348_4 = "();\n"; long long tb_c1348_4n = sizeof("();\n") - 1; char *tb_c1348r = tb_mem(tb_c1348_0n + tb_c1348_1n + tb_c1348_2n + tb_c1348_3n + tb_c1348_4n + 1), *tb_c1348rw = tb_c1348r; memcpy(tb_c1348rw, tb_c1348_0, tb_c1348_0n); tb_c1348rw += tb_c1348_0n; memcpy(tb_c1348rw, tb_c1348_1, tb_c1348_1n); tb_c1348rw += tb_c1348_1n; memcpy(tb_c1348rw, tb_c1348_2, tb_c1348_2n); tb_c1348rw += tb_c1348_2n; memcpy(tb_c1348rw, tb_c1348_3, tb_c1348_3n); tb_c1348rw += tb_c1348_3n; memcpy(tb_c1348rw, tb_c1348_4, tb_c1348_4n); tb_c1348rw += tb_c1348_4n; *tb_c1348rw = 0; (const char *)tb_c1348r; }));
    {
        long long tb_end1349 = ((((tb_o_Node *)tb_nn(v_o, 1965))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1349; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1350_0 = "    l->q_"; long long tb_c1350_0n = sizeof("    l->q_") - 1; const char *tb_c1350_1 = ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1966))->q_kids, v_i, sizeof(tb_o_Node *), 1966)), 1966))->q_name; long long tb_c1350_1n = (long long)strlen(tb_c1350_1); const char *tb_c1350_2 = "[0] = a"; long long tb_c1350_2n = sizeof("[0] = a") - 1; char tb_c1350_3d[24]; long long tb_c1350_3n; const char *tb_c1350_3 = tb_digits(v_i, tb_c1350_3d, &tb_c1350_3n); const char *tb_c1350_4 = ";\n"; long long tb_c1350_4n = sizeof(";\n") - 1; char *tb_c1350r = tb_mem(tb_c1350_0n + tb_c1350_1n + tb_c1350_2n + tb_c1350_3n + tb_c1350_4n + 1), *tb_c1350rw = tb_c1350r; memcpy(tb_c1350rw, tb_c1350_0, tb_c1350_0n); tb_c1350rw += tb_c1350_0n; memcpy(tb_c1350rw, tb_c1350_1, tb_c1350_1n); tb_c1350rw += tb_c1350_1n; memcpy(tb_c1350rw, tb_c1350_2, tb_c1350_2n); tb_c1350rw += tb_c1350_2n; memcpy(tb_c1350rw, tb_c1350_3, tb_c1350_3n); tb_c1350rw += tb_c1350_3n; memcpy(tb_c1350rw, tb_c1350_4, tb_c1350_4n); tb_c1350rw += tb_c1350_4n; *tb_c1350rw = 0; (const char *)tb_c1350r; }));
        }
    }
    f_emit__put(v_c, ({ const char *tb_c1351_0 = "    l->len = 1;\n    return ("; long long tb_c1351_0n = sizeof("    l->len = 1;\n    return (") - 1; const char *tb_c1351_1 = v_row; long long tb_c1351_1n = (long long)strlen(tb_c1351_1); const char *tb_c1351_2 = "){l, 0};\n}\n"; long long tb_c1351_2n = sizeof("){l, 0};\n}\n") - 1; char *tb_c1351r = tb_mem(tb_c1351_0n + tb_c1351_1n + tb_c1351_2n + 1), *tb_c1351rw = tb_c1351r; memcpy(tb_c1351rw, tb_c1351_0, tb_c1351_0n); tb_c1351rw += tb_c1351_0n; memcpy(tb_c1351rw, tb_c1351_1, tb_c1351_1n); tb_c1351rw += tb_c1351_1n; memcpy(tb_c1351rw, tb_c1351_2, tb_c1351_2n); tb_c1351rw += tb_c1351_2n; *tb_c1351rw = 0; (const char *)tb_c1351r; }));
}

static void f_emit__emit_constructor(tb_o_Comp * v_c, tb_o_Node * v_o) {
    if (((tb_o_Node *)tb_nn(v_o, 1970))->q_by_cols) {
        f_emit__emit_cols_helpers(v_c, v_o);
        return;
    }
    f_emit__put(v_c, ({ const char *tb_c1352_0 = "static tb_o_"; long long tb_c1352_0n = sizeof("static tb_o_") - 1; const char *tb_c1352_1 = ((tb_o_Node *)tb_nn(v_o, 1973))->q_name; long long tb_c1352_1n = (long long)strlen(tb_c1352_1); const char *tb_c1352_2 = " *tb_new_"; long long tb_c1352_2n = sizeof(" *tb_new_") - 1; const char *tb_c1352_3 = ((tb_o_Node *)tb_nn(v_o, 1973))->q_name; long long tb_c1352_3n = (long long)strlen(tb_c1352_3); const char *tb_c1352_4 = "("; long long tb_c1352_4n = sizeof("(") - 1; char *tb_c1352r = tb_mem(tb_c1352_0n + tb_c1352_1n + tb_c1352_2n + tb_c1352_3n + tb_c1352_4n + 1), *tb_c1352rw = tb_c1352r; memcpy(tb_c1352rw, tb_c1352_0, tb_c1352_0n); tb_c1352rw += tb_c1352_0n; memcpy(tb_c1352rw, tb_c1352_1, tb_c1352_1n); tb_c1352rw += tb_c1352_1n; memcpy(tb_c1352rw, tb_c1352_2, tb_c1352_2n); tb_c1352rw += tb_c1352_2n; memcpy(tb_c1352rw, tb_c1352_3, tb_c1352_3n); tb_c1352rw += tb_c1352_3n; memcpy(tb_c1352rw, tb_c1352_4, tb_c1352_4n); tb_c1352rw += tb_c1352_4n; *tb_c1352rw = 0; (const char *)tb_c1352r; }));
    if (((((tb_o_Node *)tb_nn(v_o, 1974))->q_kids)->len == 0LL)) {
        f_emit__put(v_c, "void");
    }
    {
        long long tb_end1353 = ((((tb_o_Node *)tb_nn(v_o, 1976))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1353; v_i++) {
            if ((v_i > 0LL)) {
                f_emit__put(v_c, ", ");
            }
            f_emit__put(v_c, ({ const char *tb_c1354_0 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1979))->q_kids, v_i, sizeof(tb_o_Node *), 1979)), 1979))->q_ty); long long tb_c1354_0n = (long long)strlen(tb_c1354_0); const char *tb_c1354_1 = " a"; long long tb_c1354_1n = sizeof(" a") - 1; char tb_c1354_2d[24]; long long tb_c1354_2n; const char *tb_c1354_2 = tb_digits(v_i, tb_c1354_2d, &tb_c1354_2n); char *tb_c1354r = tb_mem(tb_c1354_0n + tb_c1354_1n + tb_c1354_2n + 1), *tb_c1354rw = tb_c1354r; memcpy(tb_c1354rw, tb_c1354_0, tb_c1354_0n); tb_c1354rw += tb_c1354_0n; memcpy(tb_c1354rw, tb_c1354_1, tb_c1354_1n); tb_c1354rw += tb_c1354_1n; memcpy(tb_c1354rw, tb_c1354_2, tb_c1354_2n); tb_c1354rw += tb_c1354_2n; *tb_c1354rw = 0; (const char *)tb_c1354r; }));
        }
    }
    f_emit__put(v_c, ") {\n");
    f_emit__put(v_c, ({ const char *tb_c1355_0 = "    tb_o_"; long long tb_c1355_0n = sizeof("    tb_o_") - 1; const char *tb_c1355_1 = ((tb_o_Node *)tb_nn(v_o, 1981))->q_name; long long tb_c1355_1n = (long long)strlen(tb_c1355_1); const char *tb_c1355_2 = " *o = tb_mem(sizeof(tb_o_"; long long tb_c1355_2n = sizeof(" *o = tb_mem(sizeof(tb_o_") - 1; const char *tb_c1355_3 = ((tb_o_Node *)tb_nn(v_o, 1981))->q_name; long long tb_c1355_3n = (long long)strlen(tb_c1355_3); const char *tb_c1355_4 = "));\n"; long long tb_c1355_4n = sizeof("));\n") - 1; char *tb_c1355r = tb_mem(tb_c1355_0n + tb_c1355_1n + tb_c1355_2n + tb_c1355_3n + tb_c1355_4n + 1), *tb_c1355rw = tb_c1355r; memcpy(tb_c1355rw, tb_c1355_0, tb_c1355_0n); tb_c1355rw += tb_c1355_0n; memcpy(tb_c1355rw, tb_c1355_1, tb_c1355_1n); tb_c1355rw += tb_c1355_1n; memcpy(tb_c1355rw, tb_c1355_2, tb_c1355_2n); tb_c1355rw += tb_c1355_2n; memcpy(tb_c1355rw, tb_c1355_3, tb_c1355_3n); tb_c1355rw += tb_c1355_3n; memcpy(tb_c1355rw, tb_c1355_4, tb_c1355_4n); tb_c1355rw += tb_c1355_4n; *tb_c1355rw = 0; (const char *)tb_c1355r; }));
    if (f_emit__has_cold(v_o)) {
        f_emit__put(v_c, ({ const char *tb_c1356_0 = "    o->tb_cold = tb_cold_mem(sizeof(struct tb_c_"; long long tb_c1356_0n = sizeof("    o->tb_cold = tb_cold_mem(sizeof(struct tb_c_") - 1; const char *tb_c1356_1 = ((tb_o_Node *)tb_nn(v_o, 1983))->q_name; long long tb_c1356_1n = (long long)strlen(tb_c1356_1); const char *tb_c1356_2 = "));\n"; long long tb_c1356_2n = sizeof("));\n") - 1; char *tb_c1356r = tb_mem(tb_c1356_0n + tb_c1356_1n + tb_c1356_2n + 1), *tb_c1356rw = tb_c1356r; memcpy(tb_c1356rw, tb_c1356_0, tb_c1356_0n); tb_c1356rw += tb_c1356_0n; memcpy(tb_c1356rw, tb_c1356_1, tb_c1356_1n); tb_c1356rw += tb_c1356_1n; memcpy(tb_c1356rw, tb_c1356_2, tb_c1356_2n); tb_c1356rw += tb_c1356_2n; *tb_c1356rw = 0; (const char *)tb_c1356r; }));
    }
    {
        long long tb_end1357 = ((((tb_o_Node *)tb_nn(v_o, 1984))->q_kids)->len - 1LL);
        for (long long v_i = 0LL; v_i <= tb_end1357; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1358_0 = "    o"; long long tb_c1358_0n = sizeof("    o") - 1; const char *tb_c1358_1 = f_emit__field_path(v_o, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_o, 1985))->q_kids, v_i, sizeof(tb_o_Node *), 1985)), 1985))->q_name); long long tb_c1358_1n = (long long)strlen(tb_c1358_1); const char *tb_c1358_2 = " = a"; long long tb_c1358_2n = sizeof(" = a") - 1; char tb_c1358_3d[24]; long long tb_c1358_3n; const char *tb_c1358_3 = tb_digits(v_i, tb_c1358_3d, &tb_c1358_3n); const char *tb_c1358_4 = ";\n"; long long tb_c1358_4n = sizeof(";\n") - 1; char *tb_c1358r = tb_mem(tb_c1358_0n + tb_c1358_1n + tb_c1358_2n + tb_c1358_3n + tb_c1358_4n + 1), *tb_c1358rw = tb_c1358r; memcpy(tb_c1358rw, tb_c1358_0, tb_c1358_0n); tb_c1358rw += tb_c1358_0n; memcpy(tb_c1358rw, tb_c1358_1, tb_c1358_1n); tb_c1358rw += tb_c1358_1n; memcpy(tb_c1358rw, tb_c1358_2, tb_c1358_2n); tb_c1358rw += tb_c1358_2n; memcpy(tb_c1358rw, tb_c1358_3, tb_c1358_3n); tb_c1358rw += tb_c1358_3n; memcpy(tb_c1358rw, tb_c1358_4, tb_c1358_4n); tb_c1358rw += tb_c1358_4n; *tb_c1358rw = 0; (const char *)tb_c1358r; }));
        }
    }
    f_emit__put(v_c, "    return o;\n}\n");
}

static void f_emit__emit_c(tb_o_Comp * v_c, tb_o_Node * v_program) {
    tb_list * v_fresh = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1359 = v_fresh; tb_list * *tb_p1359 = &(((tb_o_Comp *)tb_nn(v_c, 1990))->q_out); *tb_p1359 = tb_v1359; }
    { long long tb_v1360 = 0LL; long long *tb_p1360 = &(((tb_o_Comp *)tb_nn(v_c, 1991))->q_label); *tb_p1360 = tb_v1360; }
    f_emit__put(v_c, "// Сделано компилятором Tachbit 0.7. Не правь руками: правь файл .tb\n");
    if (((tb_o_Comp *)tb_nn(v_c, 1993))->q_bare) {
        f_emit__put(v_c, "#define TB_BARE 1\n");
    }
    f_emit__put(v_c, f_runtime_text__runtime_text());
    {
        tb_list * tb_l1361 = ((tb_o_Node *)tb_nn(v_program, 1998))->q_kids;
        for (long long tb_i1361 = 0; tb_i1361 < tb_l1361->len; tb_i1361++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1361->data)[tb_i1361];
            if ((({ const char *tb_s1362 = ((tb_o_Node *)tb_nn(v_top, 1999))->q_kind; ((unsigned char)tb_s1362[0] == 111 && (unsigned char)tb_s1362[1] == 98 && (unsigned char)tb_s1362[2] == 106 && tb_s1362[3] == 0); }) && ((tb_o_Node *)tb_nn(v_top, 1999))->q_by_cols)) {
                f_emit__put(v_c, ({ const char *tb_c1363_0 = "typedef struct tb_cols_"; long long tb_c1363_0n = sizeof("typedef struct tb_cols_") - 1; const char *tb_c1363_1 = ((tb_o_Node *)tb_nn(v_top, 2000))->q_name; long long tb_c1363_1n = (long long)strlen(tb_c1363_1); const char *tb_c1363_2 = " tb_cols_"; long long tb_c1363_2n = sizeof(" tb_cols_") - 1; const char *tb_c1363_3 = ((tb_o_Node *)tb_nn(v_top, 2000))->q_name; long long tb_c1363_3n = (long long)strlen(tb_c1363_3); const char *tb_c1363_4 = ";\n"; long long tb_c1363_4n = sizeof(";\n") - 1; char *tb_c1363r = tb_mem(tb_c1363_0n + tb_c1363_1n + tb_c1363_2n + tb_c1363_3n + tb_c1363_4n + 1), *tb_c1363rw = tb_c1363r; memcpy(tb_c1363rw, tb_c1363_0, tb_c1363_0n); tb_c1363rw += tb_c1363_0n; memcpy(tb_c1363rw, tb_c1363_1, tb_c1363_1n); tb_c1363rw += tb_c1363_1n; memcpy(tb_c1363rw, tb_c1363_2, tb_c1363_2n); tb_c1363rw += tb_c1363_2n; memcpy(tb_c1363rw, tb_c1363_3, tb_c1363_3n); tb_c1363rw += tb_c1363_3n; memcpy(tb_c1363rw, tb_c1363_4, tb_c1363_4n); tb_c1363rw += tb_c1363_4n; *tb_c1363rw = 0; (const char *)tb_c1363r; }));
                f_emit__put(v_c, ({ const char *tb_c1364_0 = "typedef struct { tb_cols_"; long long tb_c1364_0n = sizeof("typedef struct { tb_cols_") - 1; const char *tb_c1364_1 = ((tb_o_Node *)tb_nn(v_top, 2001))->q_name; long long tb_c1364_1n = (long long)strlen(tb_c1364_1); const char *tb_c1364_2 = " *c; long long i; } tb_row_"; long long tb_c1364_2n = sizeof(" *c; long long i; } tb_row_") - 1; const char *tb_c1364_3 = ((tb_o_Node *)tb_nn(v_top, 2001))->q_name; long long tb_c1364_3n = (long long)strlen(tb_c1364_3); const char *tb_c1364_4 = ";\n"; long long tb_c1364_4n = sizeof(";\n") - 1; char *tb_c1364r = tb_mem(tb_c1364_0n + tb_c1364_1n + tb_c1364_2n + tb_c1364_3n + tb_c1364_4n + 1), *tb_c1364rw = tb_c1364r; memcpy(tb_c1364rw, tb_c1364_0, tb_c1364_0n); tb_c1364rw += tb_c1364_0n; memcpy(tb_c1364rw, tb_c1364_1, tb_c1364_1n); tb_c1364rw += tb_c1364_1n; memcpy(tb_c1364rw, tb_c1364_2, tb_c1364_2n); tb_c1364rw += tb_c1364_2n; memcpy(tb_c1364rw, tb_c1364_3, tb_c1364_3n); tb_c1364rw += tb_c1364_3n; memcpy(tb_c1364rw, tb_c1364_4, tb_c1364_4n); tb_c1364rw += tb_c1364_4n; *tb_c1364rw = 0; (const char *)tb_c1364r; }));
            } else if (({ const char *tb_s1365 = ((tb_o_Node *)tb_nn(v_top, 2002))->q_kind; ((unsigned char)tb_s1365[0] == 111 && (unsigned char)tb_s1365[1] == 98 && (unsigned char)tb_s1365[2] == 106 && tb_s1365[3] == 0); })) {
                f_emit__put(v_c, ({ const char *tb_c1366_0 = "typedef struct tb_o_"; long long tb_c1366_0n = sizeof("typedef struct tb_o_") - 1; const char *tb_c1366_1 = ((tb_o_Node *)tb_nn(v_top, 2003))->q_name; long long tb_c1366_1n = (long long)strlen(tb_c1366_1); const char *tb_c1366_2 = " tb_o_"; long long tb_c1366_2n = sizeof(" tb_o_") - 1; const char *tb_c1366_3 = ((tb_o_Node *)tb_nn(v_top, 2003))->q_name; long long tb_c1366_3n = (long long)strlen(tb_c1366_3); const char *tb_c1366_4 = ";\n"; long long tb_c1366_4n = sizeof(";\n") - 1; char *tb_c1366r = tb_mem(tb_c1366_0n + tb_c1366_1n + tb_c1366_2n + tb_c1366_3n + tb_c1366_4n + 1), *tb_c1366rw = tb_c1366r; memcpy(tb_c1366rw, tb_c1366_0, tb_c1366_0n); tb_c1366rw += tb_c1366_0n; memcpy(tb_c1366rw, tb_c1366_1, tb_c1366_1n); tb_c1366rw += tb_c1366_1n; memcpy(tb_c1366rw, tb_c1366_2, tb_c1366_2n); tb_c1366rw += tb_c1366_2n; memcpy(tb_c1366rw, tb_c1366_3, tb_c1366_3n); tb_c1366rw += tb_c1366_3n; memcpy(tb_c1366rw, tb_c1366_4, tb_c1366_4n); tb_c1366rw += tb_c1366_4n; *tb_c1366rw = 0; (const char *)tb_c1366r; }));
            }
        }
    }
    {
        tb_list * tb_l1367 = ((tb_o_Node *)tb_nn(v_program, 2004))->q_kids;
        for (long long tb_i1367 = 0; tb_i1367 < tb_l1367->len; tb_i1367++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1367->data)[tb_i1367];
            if (({ const char *tb_s1368 = ((tb_o_Node *)tb_nn(v_top, 2005))->q_kind; ((unsigned char)tb_s1368[0] == 111 && (unsigned char)tb_s1368[1] == 98 && (unsigned char)tb_s1368[2] == 106 && tb_s1368[3] == 0); })) {
                f_emit__emit_obj(v_c, v_top);
            }
        }
    }
    {
        tb_list * tb_l1369 = ((tb_o_Node *)tb_nn(v_program, 2008))->q_kids;
        for (long long tb_i1369 = 0; tb_i1369 < tb_l1369->len; tb_i1369++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1369->data)[tb_i1369];
            if ((({ const char *tb_s1370 = ((tb_o_Node *)tb_nn(v_top, 2009))->q_kind; ((unsigned char)tb_s1370[0] == 102 && (unsigned char)tb_s1370[1] == 117 && (unsigned char)tb_s1370[2] == 110 && (unsigned char)tb_s1370[3] == 99 && tb_s1370[4] == 0); }) && ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 2009))->q_kids, 2LL, sizeof(tb_o_Node *), 2009)), 2009))->q_kids)->len > 1LL))) {
                f_emit__put(v_c, "typedef struct {");
                {
                    long long tb_end1371 = ((((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 2011))->q_kids, 2LL, sizeof(tb_o_Node *), 2011)), 2011))->q_kids)->len - 1LL);
                    for (long long v_j = 0LL; v_j <= tb_end1371; v_j++) {
                        f_emit__put(v_c, ({ const char *tb_c1372_0 = " "; long long tb_c1372_0n = sizeof(" ") - 1; const char *tb_c1372_1 = f_emit__c_type(v_c, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_top, 2012))->q_kids, 2LL, sizeof(tb_o_Node *), 2012)), 2012))->q_kids, v_j, sizeof(tb_o_Node *), 2012)), 2012))->q_ty); long long tb_c1372_1n = (long long)strlen(tb_c1372_1); const char *tb_c1372_2 = " o"; long long tb_c1372_2n = sizeof(" o") - 1; char tb_c1372_3d[24]; long long tb_c1372_3n; const char *tb_c1372_3 = tb_digits(v_j, tb_c1372_3d, &tb_c1372_3n); const char *tb_c1372_4 = ";"; long long tb_c1372_4n = sizeof(";") - 1; char *tb_c1372r = tb_mem(tb_c1372_0n + tb_c1372_1n + tb_c1372_2n + tb_c1372_3n + tb_c1372_4n + 1), *tb_c1372rw = tb_c1372r; memcpy(tb_c1372rw, tb_c1372_0, tb_c1372_0n); tb_c1372rw += tb_c1372_0n; memcpy(tb_c1372rw, tb_c1372_1, tb_c1372_1n); tb_c1372rw += tb_c1372_1n; memcpy(tb_c1372rw, tb_c1372_2, tb_c1372_2n); tb_c1372rw += tb_c1372_2n; memcpy(tb_c1372rw, tb_c1372_3, tb_c1372_3n); tb_c1372rw += tb_c1372_3n; memcpy(tb_c1372rw, tb_c1372_4, tb_c1372_4n); tb_c1372rw += tb_c1372_4n; *tb_c1372rw = 0; (const char *)tb_c1372r; }));
                    }
                }
                f_emit__put(v_c, ({ const char *tb_c1373_0 = " } tb_r_"; long long tb_c1373_0n = sizeof(" } tb_r_") - 1; const char *tb_c1373_1 = f_emit__fname(v_top); long long tb_c1373_1n = (long long)strlen(tb_c1373_1); const char *tb_c1373_2 = ";\n"; long long tb_c1373_2n = sizeof(";\n") - 1; char *tb_c1373r = tb_mem(tb_c1373_0n + tb_c1373_1n + tb_c1373_2n + 1), *tb_c1373rw = tb_c1373r; memcpy(tb_c1373rw, tb_c1373_0, tb_c1373_0n); tb_c1373rw += tb_c1373_0n; memcpy(tb_c1373rw, tb_c1373_1, tb_c1373_1n); tb_c1373rw += tb_c1373_1n; memcpy(tb_c1373rw, tb_c1373_2, tb_c1373_2n); tb_c1373rw += tb_c1373_2n; *tb_c1373rw = 0; (const char *)tb_c1373r; }));
            }
        }
    }
    {
        tb_list * tb_l1374 = ((tb_o_Comp *)tb_nn(v_c, 2014))->q_types;
        for (long long tb_i1374 = 0; tb_i1374 < tb_l1374->len; tb_i1374++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l1374->data)[tb_i1374];
            const char * v_k = ((tb_o_Type *)tb_nn(v_t, 2015))->q_kind;
            if (((({ const char *tb_s1375 = v_k; !((unsigned char)tb_s1375[0] == 110 && (unsigned char)tb_s1375[1] == 111 && (unsigned char)tb_s1375[2] == 110 && (unsigned char)tb_s1375[3] == 101 && tb_s1375[4] == 0); }) && ({ const char *tb_s1376 = v_k; !((unsigned char)tb_s1376[0] == 109 && (unsigned char)tb_s1376[1] == 117 && (unsigned char)tb_s1376[2] == 108 && (unsigned char)tb_s1376[3] == 116 && (unsigned char)tb_s1376[4] == 105 && tb_s1376[5] == 0); })) && ({ const char *tb_s1377 = v_k; !((unsigned char)tb_s1377[0] == 117 && (unsigned char)tb_s1377[1] == 110 && (unsigned char)tb_s1377[2] == 107 && (unsigned char)tb_s1377[3] == 110 && (unsigned char)tb_s1377[4] == 111 && (unsigned char)tb_s1377[5] == 119 && (unsigned char)tb_s1377[6] == 110 && tb_s1377[7] == 0); }))) {
                f_emit__put(v_c, ({ const char *tb_c1378_0 = "static void tb_show_"; long long tb_c1378_0n = sizeof("static void tb_show_") - 1; char tb_c1378_1d[24]; long long tb_c1378_1n; const char *tb_c1378_1 = tb_digits(((tb_o_Type *)tb_nn(v_t, 2017))->q_id, tb_c1378_1d, &tb_c1378_1n); const char *tb_c1378_2 = "("; long long tb_c1378_2n = sizeof("(") - 1; const char *tb_c1378_3 = f_emit__c_type(v_c, ((tb_o_Type *)tb_nn(v_t, 2017))->q_id); long long tb_c1378_3n = (long long)strlen(tb_c1378_3); const char *tb_c1378_4 = " x);\n"; long long tb_c1378_4n = sizeof(" x);\n") - 1; char *tb_c1378r = tb_mem(tb_c1378_0n + tb_c1378_1n + tb_c1378_2n + tb_c1378_3n + tb_c1378_4n + 1), *tb_c1378rw = tb_c1378r; memcpy(tb_c1378rw, tb_c1378_0, tb_c1378_0n); tb_c1378rw += tb_c1378_0n; memcpy(tb_c1378rw, tb_c1378_1, tb_c1378_1n); tb_c1378rw += tb_c1378_1n; memcpy(tb_c1378rw, tb_c1378_2, tb_c1378_2n); tb_c1378rw += tb_c1378_2n; memcpy(tb_c1378rw, tb_c1378_3, tb_c1378_3n); tb_c1378rw += tb_c1378_3n; memcpy(tb_c1378rw, tb_c1378_4, tb_c1378_4n); tb_c1378rw += tb_c1378_4n; *tb_c1378rw = 0; (const char *)tb_c1378r; }));
            }
        }
    }
    {
        long long tb_end1379 = ((((tb_o_Comp *)tb_nn(v_c, 2018))->q_temp_types)->len - 1LL);
        for (long long v_i = 1LL; v_i <= tb_end1379; v_i++) {
            f_emit__put(v_c, ({ const char *tb_c1380_0 = "static _Thread_local "; long long tb_c1380_0n = sizeof("static _Thread_local ") - 1; const char *tb_c1380_1 = f_emit__c_type(v_c, (*(long long *)tb_at(((tb_o_Comp *)tb_nn(v_c, 2019))->q_temp_types, v_i, sizeof(long long), 2019))); long long tb_c1380_1n = (long long)strlen(tb_c1380_1); const char *tb_c1380_2 = " tb_t"; long long tb_c1380_2n = sizeof(" tb_t") - 1; char tb_c1380_3d[24]; long long tb_c1380_3n; const char *tb_c1380_3 = tb_digits(v_i, tb_c1380_3d, &tb_c1380_3n); const char *tb_c1380_4 = ";   // для  f() or запасное\n"; long long tb_c1380_4n = sizeof(";   // для  f() or запасное\n") - 1; char *tb_c1380r = tb_mem(tb_c1380_0n + tb_c1380_1n + tb_c1380_2n + tb_c1380_3n + tb_c1380_4n + 1), *tb_c1380rw = tb_c1380r; memcpy(tb_c1380rw, tb_c1380_0, tb_c1380_0n); tb_c1380rw += tb_c1380_0n; memcpy(tb_c1380rw, tb_c1380_1, tb_c1380_1n); tb_c1380rw += tb_c1380_1n; memcpy(tb_c1380rw, tb_c1380_2, tb_c1380_2n); tb_c1380rw += tb_c1380_2n; memcpy(tb_c1380rw, tb_c1380_3, tb_c1380_3n); tb_c1380rw += tb_c1380_3n; memcpy(tb_c1380rw, tb_c1380_4, tb_c1380_4n); tb_c1380rw += tb_c1380_4n; *tb_c1380rw = 0; (const char *)tb_c1380r; }));
        }
    }
    {
        tb_list * tb_l1381 = ((tb_o_Node *)tb_nn(v_program, 2020))->q_kids;
        for (long long tb_i1381 = 0; tb_i1381 < tb_l1381->len; tb_i1381++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1381->data)[tb_i1381];
            if (({ const char *tb_s1382 = ((tb_o_Node *)tb_nn(v_top, 2021))->q_kind; ((unsigned char)tb_s1382[0] == 111 && (unsigned char)tb_s1382[1] == 98 && (unsigned char)tb_s1382[2] == 106 && tb_s1382[3] == 0); })) {
                f_emit__emit_constructor(v_c, v_top);
            }
        }
    }
    {
        tb_list * tb_l1383 = ((tb_o_Node *)tb_nn(v_program, 2023))->q_kids;
        for (long long tb_i1383 = 0; tb_i1383 < tb_l1383->len; tb_i1383++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1383->data)[tb_i1383];
            if (({ const char *tb_s1384 = ((tb_o_Node *)tb_nn(v_top, 2024))->q_kind; ((unsigned char)tb_s1384[0] == 102 && (unsigned char)tb_s1384[1] == 117 && (unsigned char)tb_s1384[2] == 110 && (unsigned char)tb_s1384[3] == 99 && tb_s1384[4] == 0); })) {
                f_emit__emit_signature(v_c, v_top);
                f_emit__put(v_c, ";\n");
            }
        }
    }
    f_emit__put(v_c, "\n");
    {
        tb_list * tb_l1385 = ((tb_o_Comp *)tb_nn(v_c, 2028))->q_types;
        for (long long tb_i1385 = 0; tb_i1385 < tb_l1385->len; tb_i1385++) {
            tb_o_Type * const v_t = ((tb_o_Type * *)tb_l1385->data)[tb_i1385];
            f_emit__emit_show(v_c, v_t);
        }
    }
    tb_list * v_saved = ((tb_o_Comp *)tb_nn(v_c, 2031))->q_out;
    tb_list * v_funcs_out = tb_list_new(sizeof(const char *));
    { tb_list * tb_v1386 = v_funcs_out; tb_list * *tb_p1386 = &(((tb_o_Comp *)tb_nn(v_c, 2033))->q_out); *tb_p1386 = tb_v1386; }
    {
        tb_list * tb_l1387 = ((tb_o_Node *)tb_nn(v_program, 2034))->q_kids;
        for (long long tb_i1387 = 0; tb_i1387 < tb_l1387->len; tb_i1387++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1387->data)[tb_i1387];
            if (({ const char *tb_s1388 = ((tb_o_Node *)tb_nn(v_top, 2035))->q_kind; ((unsigned char)tb_s1388[0] == 102 && (unsigned char)tb_s1388[1] == 117 && (unsigned char)tb_s1388[2] == 110 && (unsigned char)tb_s1388[3] == 99 && tb_s1388[4] == 0); })) {
                f_emit__emit_func(v_c, v_top);
            }
        }
    }
    { tb_list * tb_v1389 = v_saved; tb_list * *tb_p1389 = &(((tb_o_Comp *)tb_nn(v_c, 2037))->q_out); *tb_p1389 = tb_v1389; }
    {
        tb_list * tb_l1390 = ((tb_o_Comp *)tb_nn(v_c, 2038))->q_workers;
        for (long long tb_i1390 = 0; tb_i1390 < tb_l1390->len; tb_i1390++) {
            const char * const v_w = ((const char * *)tb_l1390->data)[tb_i1390];
            f_emit__put(v_c, v_w);
        }
    }
    {
        tb_list * tb_l1391 = v_funcs_out;
        for (long long tb_i1391 = 0; tb_i1391 < tb_l1391->len; tb_i1391++) {
            const char * const v_f = ((const char * *)tb_l1391->data)[tb_i1391];
            f_emit__put(v_c, v_f);
        }
    }
    tb_o_Node * v_main_func = (*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 2042))->q_kids, 0LL, sizeof(tb_o_Node *), 2042));
    {
        tb_list * tb_l1392 = ((tb_o_Node *)tb_nn(v_program, 2043))->q_kids;
        for (long long tb_i1392 = 0; tb_i1392 < tb_l1392->len; tb_i1392++) {
            tb_o_Node * const v_top = ((tb_o_Node * *)tb_l1392->data)[tb_i1392];
            if (((({ const char *tb_s1393 = ((tb_o_Node *)tb_nn(v_top, 2044))->q_kind; ((unsigned char)tb_s1393[0] == 102 && (unsigned char)tb_s1393[1] == 117 && (unsigned char)tb_s1393[2] == 110 && (unsigned char)tb_s1393[3] == 99 && tb_s1393[4] == 0); }) && ({ const char *tb_s1394 = ((tb_o_Node *)tb_nn(v_top, 2044))->q_name; ((unsigned char)tb_s1394[0] == 109 && (unsigned char)tb_s1394[1] == 97 && (unsigned char)tb_s1394[2] == 105 && (unsigned char)tb_s1394[3] == 110 && tb_s1394[4] == 0); })) && (strcmp(((tb_o_Node *)tb_nn(v_top, 2044))->q_mod, ((tb_o_Node *)tb_nn((*(tb_o_Node * *)tb_at(((tb_o_Node *)tb_nn(v_program, 2044))->q_kids, 0LL, sizeof(tb_o_Node *), 2044)), 2044))->q_mod) == 0))) {
                v_main_func = v_top;
            }
        }
    }
    if (((tb_o_Comp *)tb_nn(v_c, 2046))->q_bare) {
        f_emit__put(v_c, "\nvoid tb_kmain(void) {\n    tb_args = tb_list_new(sizeof(const char *));\n");
        f_emit__put(v_c, ({ const char *tb_c1395_0 = "    "; long long tb_c1395_0n = sizeof("    ") - 1; const char *tb_c1395_1 = f_emit__fname(v_main_func); long long tb_c1395_1n = (long long)strlen(tb_c1395_1); const char *tb_c1395_2 = "();\n}\n"; long long tb_c1395_2n = sizeof("();\n}\n") - 1; char *tb_c1395r = tb_mem(tb_c1395_0n + tb_c1395_1n + tb_c1395_2n + 1), *tb_c1395rw = tb_c1395r; memcpy(tb_c1395rw, tb_c1395_0, tb_c1395_0n); tb_c1395rw += tb_c1395_0n; memcpy(tb_c1395rw, tb_c1395_1, tb_c1395_1n); tb_c1395rw += tb_c1395_1n; memcpy(tb_c1395rw, tb_c1395_2, tb_c1395_2n); tb_c1395rw += tb_c1395_2n; *tb_c1395rw = 0; (const char *)tb_c1395r; }));
        return;
    }
    f_emit__put(v_c, "\nint main(int argc, char **argv) {\n");
    f_emit__put(v_c, "    tb_args = tb_list_new(sizeof(const char *));\n");
    f_emit__put(v_c, "    for (int i = 1; i < argc; i++) *(const char **)tb_grow(tb_args) = argv[i];\n");
    f_emit__put(v_c, ({ const char *tb_c1396_0 = "    "; long long tb_c1396_0n = sizeof("    ") - 1; const char *tb_c1396_1 = f_emit__fname(v_main_func); long long tb_c1396_1n = (long long)strlen(tb_c1396_1); const char *tb_c1396_2 = "();\n    return 0;\n}\n"; long long tb_c1396_2n = sizeof("();\n    return 0;\n}\n") - 1; char *tb_c1396r = tb_mem(tb_c1396_0n + tb_c1396_1n + tb_c1396_2n + 1), *tb_c1396rw = tb_c1396r; memcpy(tb_c1396rw, tb_c1396_0, tb_c1396_0n); tb_c1396rw += tb_c1396_0n; memcpy(tb_c1396rw, tb_c1396_1, tb_c1396_1n); tb_c1396rw += tb_c1396_1n; memcpy(tb_c1396rw, tb_c1396_2, tb_c1396_2n); tb_c1396rw += tb_c1396_2n; *tb_c1396rw = 0; (const char *)tb_c1396r; }));
}

static const char * f_runtime_text__runtime_text(void) {
    tb_list * v_lines = tb_list_new(sizeof(const char *));
    { const char * tb_v1397 = "// Библиотека, которую Tachbit кладёт в начало каждой программы на C.\n"; *(const char * *)tb_grow(v_lines) = tb_v1397; }
    { const char * tb_v1398 = "// Это обычный C. Компилятор на Tachbit встраивает этот текст через runtime_text.tb,\n"; *(const char * *)tb_grow(v_lines) = tb_v1398; }
    { const char * tb_v1399 = "// который делает tools/embed_runtime.py. Поменял этот файл — запусти:\n"; *(const char * *)tb_grow(v_lines) = tb_v1399; }
    { const char * tb_v1400 = "//     python3 tools/embed_runtime.py\n"; *(const char * *)tb_grow(v_lines) = tb_v1400; }
    { const char * tb_v1401 = "#ifdef TB_BARE\n"; *(const char * *)tb_grow(v_lines) = tb_v1401; }
    { const char * tb_v1402 = "// ---------- режим без ОС (tb bare): своя маленькая библиотека вместо libc ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1402; }
    { const char * tb_v1403 = "// Программа сама себе ОС: загружается через multiboot (QEMU, GRUB), пишет на экран\n"; *(const char * *)tb_grow(v_lines) = tb_v1403; }
    { const char * tb_v1404 = "// (текстовый режим VGA) и в последовательный порт COM1, память берёт из своего запаса.\n"; *(const char * *)tb_grow(v_lines) = tb_v1404; }
    { const char * tb_v1405 = "// Ядер, файлов и ввода здесь нет: on all cores и background работают на одном ядре.\n"; *(const char * *)tb_grow(v_lines) = tb_v1405; }
    { const char * tb_v1406 = "#include <stddef.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1406; }
    { const char * tb_v1407 = "#include <stdarg.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1407; }
    { const char * tb_v1408 = "#define _Thread_local\n"; *(const char * *)tb_grow(v_lines) = tb_v1408; }
    { const char * tb_v1409 = "#define EOF (-1)\n"; *(const char * *)tb_grow(v_lines) = tb_v1409; }
    { const char * tb_v1410 = "typedef struct tb_file FILE;\n"; *(const char * *)tb_grow(v_lines) = tb_v1410; }
    { const char * tb_v1411 = "#define stdout ((FILE *)1)\n"; *(const char * *)tb_grow(v_lines) = tb_v1411; }
    { const char * tb_v1412 = "#define stderr ((FILE *)2)\n"; *(const char * *)tb_grow(v_lines) = tb_v1412; }
    { const char * tb_v1413 = "typedef int pthread_t;\n"; *(const char * *)tb_grow(v_lines) = tb_v1413; }
    { const char * tb_v1414 = "typedef int pthread_mutex_t;\n"; *(const char * *)tb_grow(v_lines) = tb_v1414; }
    { const char * tb_v1415 = "#define PTHREAD_MUTEX_INITIALIZER 0\n"; *(const char * *)tb_grow(v_lines) = tb_v1415; }
    { const char * tb_v1416 = "#define pthread_mutex_lock(m) ((void)(m))\n"; *(const char * *)tb_grow(v_lines) = tb_v1416; }
    { const char * tb_v1417 = "#define pthread_mutex_unlock(m) ((void)(m))\n"; *(const char * *)tb_grow(v_lines) = tb_v1417; }
    { const char * tb_v1418 = "#define _SC_NPROCESSORS_ONLN 0\n"; *(const char * *)tb_grow(v_lines) = tb_v1418; }
    { const char * tb_v1419 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1419; }
    { const char * tb_v1420 = "static inline void tb_outb(unsigned short port, unsigned char v) { __asm__ volatile(\"outb %0, %1\" : : \"a\"(v), \"Nd\"(port)); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1420; }
    { const char * tb_v1421 = "static inline unsigned char tb_inb(unsigned short port) { unsigned char v; __asm__ volatile(\"inb %1, %0\" : \"=a\"(v) : \"Nd\"(port)); return v; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1421; }
    { const char * tb_v1422 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1422; }
    { const char * tb_v1423 = "// экран 80×25: байт буквы и байт цвета; русские буквы VGA не умеет — на экране «?», в порту как есть\n"; *(const char * *)tb_grow(v_lines) = tb_v1423; }
    { const char * tb_v1424 = "static volatile unsigned short *const tb_vga = (volatile unsigned short *)0xB8000;\n"; *(const char * *)tb_grow(v_lines) = tb_v1424; }
    { const char * tb_v1425 = "static int tb_vrow = 0, tb_vcol = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1425; }
    { const char * tb_v1426 = "static unsigned char tb_vcolor = 0x0A;   // светло-зелёный на чёрном\n"; *(const char * *)tb_grow(v_lines) = tb_v1426; }
    { const char * tb_v1427 = "static void tb_vga_put(unsigned char ch) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1427; }
    { const char * tb_v1428 = "    if (ch == '\\n') { tb_vcol = 0; tb_vrow++; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1428; }
    { const char * tb_v1429 = "    else {\n"; *(const char * *)tb_grow(v_lines) = tb_v1429; }
    { const char * tb_v1430 = "        tb_vga[tb_vrow * 80 + tb_vcol] = (unsigned short)(tb_vcolor << 8 | ch);\n"; *(const char * *)tb_grow(v_lines) = tb_v1430; }
    { const char * tb_v1431 = "        if (++tb_vcol == 80) { tb_vcol = 0; tb_vrow++; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1431; }
    { const char * tb_v1432 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1432; }
    { const char * tb_v1433 = "    if (tb_vrow == 25) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1433; }
    { const char * tb_v1434 = "        for (int i = 0; i < 24 * 80; i++) tb_vga[i] = tb_vga[i + 80];\n"; *(const char * *)tb_grow(v_lines) = tb_v1434; }
    { const char * tb_v1435 = "        for (int i = 24 * 80; i < 25 * 80; i++) tb_vga[i] = (unsigned short)(tb_vcolor << 8 | ' ');\n"; *(const char * *)tb_grow(v_lines) = tb_v1435; }
    { const char * tb_v1436 = "        tb_vrow = 24;\n"; *(const char * *)tb_grow(v_lines) = tb_v1436; }
    { const char * tb_v1437 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1437; }
    { const char * tb_v1438 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1438; }
    { const char * tb_v1439 = "static void tb_putc(char c) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1439; }
    { const char * tb_v1440 = "    unsigned char ch = (unsigned char)c;\n"; *(const char * *)tb_grow(v_lines) = tb_v1440; }
    { const char * tb_v1441 = "    while ((tb_inb(0x3F8 + 5) & 0x20) == 0) {}\n"; *(const char * *)tb_grow(v_lines) = tb_v1441; }
    { const char * tb_v1442 = "    tb_outb(0x3F8, ch);\n"; *(const char * *)tb_grow(v_lines) = tb_v1442; }
    { const char * tb_v1443 = "    if (ch < 0x80) tb_vga_put(ch);\n"; *(const char * *)tb_grow(v_lines) = tb_v1443; }
    { const char * tb_v1444 = "    else if (ch >= 0xC0) tb_vga_put('?');   // первый байт русской буквы (UTF-8) → «?», остальные пропускаем\n"; *(const char * *)tb_grow(v_lines) = tb_v1444; }
    { const char * tb_v1445 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1445; }
    { const char * tb_v1446 = "static int fputs(const char *s, FILE *f) { (void)f; while (*s) tb_putc(*s++); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1446; }
    { const char * tb_v1447 = "static int fflush(FILE *f) { (void)f; return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1447; }
    { const char * tb_v1448 = "static int isatty(int fd) { (void)fd; return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1448; }
    { const char * tb_v1449 = "static char *getenv(const char *n) { (void)n; return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1449; }
    { const char * tb_v1450 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1450; }
    { const char * tb_v1451 = "// выход: QEMU с -device isa-debug-exit закрывается, без него процессор просто спит\n"; *(const char * *)tb_grow(v_lines) = tb_v1451; }
    { const char * tb_v1452 = "__attribute__((noreturn)) static void exit(int code) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1452; }
    { const char * tb_v1453 = "    tb_outb(0xF4, (unsigned char)code);\n"; *(const char * *)tb_grow(v_lines) = tb_v1453; }
    { const char * tb_v1454 = "    for (;;) __asm__ volatile(\"cli; hlt\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1454; }
    { const char * tb_v1455 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1455; }
    { const char * tb_v1456 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1456; }
    { const char * tb_v1457 = "// память: запас 64 МБ. Куски по размерам 16, 32, 64, ... байт; отданный кусок\n"; *(const char * *)tb_grow(v_lines) = tb_v1457; }
    { const char * tb_v1458 = "// кладётся в корзину своего размера и выдаётся снова. Последний кусок отдаётся сразу.\n"; *(const char * *)tb_grow(v_lines) = tb_v1458; }
    { const char * tb_v1459 = "#define TB_HEAP (64 << 20)\n"; *(const char * *)tb_grow(v_lines) = tb_v1459; }
    { const char * tb_v1460 = "static unsigned char tb_heap[TB_HEAP] __attribute__((aligned(16)));\n"; *(const char * *)tb_grow(v_lines) = tb_v1460; }
    { const char * tb_v1461 = "static size_t tb_heap_top = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1461; }
    { const char * tb_v1462 = "static void *tb_bins[32];\n"; *(const char * *)tb_grow(v_lines) = tb_v1462; }
    { const char * tb_v1463 = "static int tb_cls(size_t n) { int k = 4; while (k < 31 && ((size_t)1 << k) < n) k++; return k; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1463; }
    { const char * tb_v1464 = "static void *malloc(size_t n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1464; }
    { const char * tb_v1465 = "    int k = tb_cls(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1465; }
    { const char * tb_v1466 = "    if (tb_bins[k]) { void *p = tb_bins[k]; tb_bins[k] = *(void **)p; return p; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1466; }
    { const char * tb_v1467 = "    size_t need = ((size_t)1 << k) + 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v1467; }
    { const char * tb_v1468 = "    if (tb_heap_top + need > TB_HEAP) return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1468; }
    { const char * tb_v1469 = "    unsigned char *h = tb_heap + tb_heap_top;\n"; *(const char * *)tb_grow(v_lines) = tb_v1469; }
    { const char * tb_v1470 = "    *(size_t *)h = (size_t)k;\n"; *(const char * *)tb_grow(v_lines) = tb_v1470; }
    { const char * tb_v1471 = "    tb_heap_top += need;\n"; *(const char * *)tb_grow(v_lines) = tb_v1471; }
    { const char * tb_v1472 = "    return h + 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v1472; }
    { const char * tb_v1473 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1473; }
    { const char * tb_v1474 = "static void free(void *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1474; }
    { const char * tb_v1475 = "    if (p == 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1475; }
    { const char * tb_v1476 = "    unsigned char *h = (unsigned char *)p - 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v1476; }
    { const char * tb_v1477 = "    int k = (int)*(size_t *)h;\n"; *(const char * *)tb_grow(v_lines) = tb_v1477; }
    { const char * tb_v1478 = "    if (h + ((size_t)1 << k) + 16 == tb_heap + tb_heap_top) { tb_heap_top = (size_t)(h - tb_heap); return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1478; }
    { const char * tb_v1479 = "    *(void **)p = tb_bins[k];\n"; *(const char * *)tb_grow(v_lines) = tb_v1479; }
    { const char * tb_v1480 = "    tb_bins[k] = p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1480; }
    { const char * tb_v1481 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1481; }
    { const char * tb_v1482 = "void *memcpy(void *d, const void *s, size_t n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1482; }
    { const char * tb_v1483 = "    void *r = d;\n"; *(const char * *)tb_grow(v_lines) = tb_v1483; }
    { const char * tb_v1484 = "    __asm__ volatile(\"rep movsb\" : \"+D\"(d), \"+S\"(s), \"+c\"(n) : : \"memory\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1484; }
    { const char * tb_v1485 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1485; }
    { const char * tb_v1486 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1486; }
    { const char * tb_v1487 = "void *memmove(void *d, const void *s, size_t n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1487; }
    { const char * tb_v1488 = "    unsigned char *a = d; const unsigned char *b = s;\n"; *(const char * *)tb_grow(v_lines) = tb_v1488; }
    { const char * tb_v1489 = "    if (a < b) while (n--) *a++ = *b++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1489; }
    { const char * tb_v1490 = "    else { a += n; b += n; while (n--) *--a = *--b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1490; }
    { const char * tb_v1491 = "    return d;\n"; *(const char * *)tb_grow(v_lines) = tb_v1491; }
    { const char * tb_v1492 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1492; }
    { const char * tb_v1493 = "void *memset(void *d, int v, size_t n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1493; }
    { const char * tb_v1494 = "    void *r = d;\n"; *(const char * *)tb_grow(v_lines) = tb_v1494; }
    { const char * tb_v1495 = "    __asm__ volatile(\"rep stosb\" : \"+D\"(d), \"+c\"(n) : \"a\"(v) : \"memory\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1495; }
    { const char * tb_v1496 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1496; }
    { const char * tb_v1497 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1497; }
    { const char * tb_v1498 = "int memcmp(const void *x, const void *y, size_t n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1498; }
    { const char * tb_v1499 = "    const unsigned char *a = x, *b = y;\n"; *(const char * *)tb_grow(v_lines) = tb_v1499; }
    { const char * tb_v1500 = "    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;\n"; *(const char * *)tb_grow(v_lines) = tb_v1500; }
    { const char * tb_v1501 = "    return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1501; }
    { const char * tb_v1502 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1502; }
    { const char * tb_v1503 = "static void *realloc(void *p, size_t n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1503; }
    { const char * tb_v1504 = "    if (p == 0) return malloc(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1504; }
    { const char * tb_v1505 = "    unsigned char *h = (unsigned char *)p - 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v1505; }
    { const char * tb_v1506 = "    int k = (int)*(size_t *)h;\n"; *(const char * *)tb_grow(v_lines) = tb_v1506; }
    { const char * tb_v1507 = "    size_t cap = (size_t)1 << k;\n"; *(const char * *)tb_grow(v_lines) = tb_v1507; }
    { const char * tb_v1508 = "    if (n <= cap) return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1508; }
    { const char * tb_v1509 = "    // последний кусок растёт на месте\n"; *(const char * *)tb_grow(v_lines) = tb_v1509; }
    { const char * tb_v1510 = "    if (h + cap + 16 == tb_heap + tb_heap_top) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1510; }
    { const char * tb_v1511 = "        int k2 = tb_cls(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1511; }
    { const char * tb_v1512 = "        size_t need = ((size_t)1 << k2) + 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v1512; }
    { const char * tb_v1513 = "        if ((size_t)(h - tb_heap) + need <= TB_HEAP) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1513; }
    { const char * tb_v1514 = "            *(size_t *)h = (size_t)k2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1514; }
    { const char * tb_v1515 = "            tb_heap_top = (size_t)(h - tb_heap) + need;\n"; *(const char * *)tb_grow(v_lines) = tb_v1515; }
    { const char * tb_v1516 = "            return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1516; }
    { const char * tb_v1517 = "        }\n"; *(const char * *)tb_grow(v_lines) = tb_v1517; }
    { const char * tb_v1518 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1518; }
    { const char * tb_v1519 = "    void *q = malloc(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1519; }
    { const char * tb_v1520 = "    if (q) { memcpy(q, p, cap); free(p); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1520; }
    { const char * tb_v1521 = "    return q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1521; }
    { const char * tb_v1522 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1522; }
    { const char * tb_v1523 = "// одно ядро и нет прерываний: «атомарные» действия — обычные\n"; *(const char * *)tb_grow(v_lines) = tb_v1523; }
    { const char * tb_v1524 = "unsigned long long __atomic_fetch_add_8(volatile void *p, unsigned long long v, int order) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1524; }
    { const char * tb_v1525 = "    (void)order;\n"; *(const char * *)tb_grow(v_lines) = tb_v1525; }
    { const char * tb_v1526 = "    unsigned long long old = *(volatile unsigned long long *)p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1526; }
    { const char * tb_v1527 = "    *(volatile unsigned long long *)p = old + v;\n"; *(const char * *)tb_grow(v_lines) = tb_v1527; }
    { const char * tb_v1528 = "    return old;\n"; *(const char * *)tb_grow(v_lines) = tb_v1528; }
    { const char * tb_v1529 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1529; }
    { const char * tb_v1530 = "size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1530; }
    { const char * tb_v1531 = "int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1531; }
    { const char * tb_v1532 = "static int strncmp(const char *a, const char *b, size_t n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1532; }
    { const char * tb_v1533 = "    for (; n; n--, a++, b++) { if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1533; }
    { const char * tb_v1534 = "    return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1534; }
    { const char * tb_v1535 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1535; }
    { const char * tb_v1536 = "static char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1536; }
    { const char * tb_v1537 = "static char *strstr(const char *h, const char *n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1537; }
    { const char * tb_v1538 = "    size_t ln = strlen(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1538; }
    { const char * tb_v1539 = "    if (ln == 0) return (char *)h;\n"; *(const char * *)tb_grow(v_lines) = tb_v1539; }
    { const char * tb_v1540 = "    for (; *h; h++) if (*h == *n && strncmp(h, n, ln) == 0) return (char *)h;\n"; *(const char * *)tb_grow(v_lines) = tb_v1540; }
    { const char * tb_v1541 = "    return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1541; }
    { const char * tb_v1542 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1542; }
    { const char * tb_v1543 = "static long atol(const char *s) { long r = 0; while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0'); return r; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1543; }
    { const char * tb_v1544 = "static double sqrt(double x) { double r; __asm__(\"fsqrt\" : \"=t\"(r) : \"0\"(x)); return r; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1544; }
    { const char * tb_v1545 = "static double fabs(double x) { return x < 0 ? -x : x; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1545; }
    { const char * tb_v1546 = "static double strtod(const char *s, char **end) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1546; }
    { const char * tb_v1547 = "    double r = 0, sign = 1, scale = 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1547; }
    { const char * tb_v1548 = "    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1548; }
    { const char * tb_v1549 = "    while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');\n"; *(const char * *)tb_grow(v_lines) = tb_v1549; }
    { const char * tb_v1550 = "    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { r = r * 10 + (*s++ - '0'); scale *= 10; } }\n"; *(const char * *)tb_grow(v_lines) = tb_v1550; }
    { const char * tb_v1551 = "    if (end) *end = (char *)s;\n"; *(const char * *)tb_grow(v_lines) = tb_v1551; }
    { const char * tb_v1552 = "    return sign * r / scale;\n"; *(const char * *)tb_grow(v_lines) = tb_v1552; }
    { const char * tb_v1553 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1553; }
    { const char * tb_v1554 = "// деление 64-битных чисел на 32-битном процессоре: этих функций без libgcc нет\n"; *(const char * *)tb_grow(v_lines) = tb_v1554; }
    { const char * tb_v1555 = "unsigned long long __udivmoddi4(unsigned long long a, unsigned long long b, unsigned long long *rem) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1555; }
    { const char * tb_v1556 = "    unsigned long long q = 0, r = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1556; }
    { const char * tb_v1557 = "    for (int i = 63; i >= 0; i--) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1557; }
    { const char * tb_v1558 = "        r = (r << 1) | ((a >> i) & 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1558; }
    { const char * tb_v1559 = "        if (r >= b) { r -= b; q |= 1ULL << i; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1559; }
    { const char * tb_v1560 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1560; }
    { const char * tb_v1561 = "    if (rem) *rem = r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1561; }
    { const char * tb_v1562 = "    return q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1562; }
    { const char * tb_v1563 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1563; }
    { const char * tb_v1564 = "unsigned long long __udivdi3(unsigned long long a, unsigned long long b) { return __udivmoddi4(a, b, 0); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1564; }
    { const char * tb_v1565 = "unsigned long long __umoddi3(unsigned long long a, unsigned long long b) { unsigned long long r; __udivmoddi4(a, b, &r); return r; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1565; }
    { const char * tb_v1566 = "long long __divdi3(long long a, long long b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1566; }
    { const char * tb_v1567 = "    int neg = (a < 0) != (b < 0);\n"; *(const char * *)tb_grow(v_lines) = tb_v1567; }
    { const char * tb_v1568 = "    unsigned long long q = __udivmoddi4(a < 0 ? 0 - (unsigned long long)a : (unsigned long long)a, b < 0 ? 0 - (unsigned long long)b : (unsigned long long)b, 0);\n"; *(const char * *)tb_grow(v_lines) = tb_v1568; }
    { const char * tb_v1569 = "    return neg ? -(long long)q : (long long)q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1569; }
    { const char * tb_v1570 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1570; }
    { const char * tb_v1571 = "long long __moddi3(long long a, long long b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1571; }
    { const char * tb_v1572 = "    unsigned long long r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1572; }
    { const char * tb_v1573 = "    __udivmoddi4(a < 0 ? 0 - (unsigned long long)a : (unsigned long long)a, b < 0 ? 0 - (unsigned long long)b : (unsigned long long)b, &r);\n"; *(const char * *)tb_grow(v_lines) = tb_v1573; }
    { const char * tb_v1574 = "    return a < 0 ? -(long long)r : (long long)r;\n"; *(const char * *)tb_grow(v_lines) = tb_v1574; }
    { const char * tb_v1575 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1575; }
    { const char * tb_v1576 = "// маленький printf: %lld %ld %d %s %c %g (дробь — до 6 знаков после точки)\n"; *(const char * *)tb_grow(v_lines) = tb_v1576; }
    { const char * tb_v1577 = "static int tb_vfmt(char *out, size_t cap, const char *f, va_list ap) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1577; }
    { const char * tb_v1578 = "    size_t n = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1578; }
    { const char * tb_v1579 = "#define TB_O(ch) do { if (n + 1 < cap) out[n] = (ch); n++; } while (0)\n"; *(const char * *)tb_grow(v_lines) = tb_v1579; }
    { const char * tb_v1580 = "    for (; *f; f++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1580; }
    { const char * tb_v1581 = "        if (*f != '%') { TB_O(*f); continue; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1581; }
    { const char * tb_v1582 = "        f++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1582; }
    { const char * tb_v1583 = "        int ll = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1583; }
    { const char * tb_v1584 = "        while (*f == 'l') { ll++; f++; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1584; }
    { const char * tb_v1585 = "        if (*f == 's') { const char *s = va_arg(ap, const char *); while (*s) TB_O(*s++); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1585; }
    { const char * tb_v1586 = "        else if (*f == 'c') TB_O((char)va_arg(ap, int));\n"; *(const char * *)tb_grow(v_lines) = tb_v1586; }
    { const char * tb_v1587 = "        else if (*f == 'd') {\n"; *(const char * *)tb_grow(v_lines) = tb_v1587; }
    { const char * tb_v1588 = "            long long v = ll >= 2 ? va_arg(ap, long long) : ll == 1 ? va_arg(ap, long) : va_arg(ap, int);\n"; *(const char * *)tb_grow(v_lines) = tb_v1588; }
    { const char * tb_v1589 = "            char d[24]; int k = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1589; }
    { const char * tb_v1590 = "            unsigned long long u = v < 0 ? 0 - (unsigned long long)v : (unsigned long long)v;\n"; *(const char * *)tb_grow(v_lines) = tb_v1590; }
    { const char * tb_v1591 = "            do { d[k++] = (char)('0' + u % 10); u /= 10; } while (u);\n"; *(const char * *)tb_grow(v_lines) = tb_v1591; }
    { const char * tb_v1592 = "            if (v < 0) TB_O('-');\n"; *(const char * *)tb_grow(v_lines) = tb_v1592; }
    { const char * tb_v1593 = "            while (k) TB_O(d[--k]);\n"; *(const char * *)tb_grow(v_lines) = tb_v1593; }
    { const char * tb_v1594 = "        } else if (*f == 'g') {\n"; *(const char * *)tb_grow(v_lines) = tb_v1594; }
    { const char * tb_v1595 = "            double x = va_arg(ap, double);\n"; *(const char * *)tb_grow(v_lines) = tb_v1595; }
    { const char * tb_v1596 = "            if (x < 0) { TB_O('-'); x = -x; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1596; }
    { const char * tb_v1597 = "            unsigned long long ip = (unsigned long long)x;\n"; *(const char * *)tb_grow(v_lines) = tb_v1597; }
    { const char * tb_v1598 = "            double fr = x - (double)ip;\n"; *(const char * *)tb_grow(v_lines) = tb_v1598; }
    { const char * tb_v1599 = "            unsigned long long fp = (unsigned long long)(fr * 1000000 + 0.5);\n"; *(const char * *)tb_grow(v_lines) = tb_v1599; }
    { const char * tb_v1600 = "            if (fp >= 1000000) { ip++; fp -= 1000000; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1600; }
    { const char * tb_v1601 = "            char d[24]; int k = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1601; }
    { const char * tb_v1602 = "            do { d[k++] = (char)('0' + ip % 10); ip /= 10; } while (ip);\n"; *(const char * *)tb_grow(v_lines) = tb_v1602; }
    { const char * tb_v1603 = "            while (k) TB_O(d[--k]);\n"; *(const char * *)tb_grow(v_lines) = tb_v1603; }
    { const char * tb_v1604 = "            if (fp) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1604; }
    { const char * tb_v1605 = "                TB_O('.');\n"; *(const char * *)tb_grow(v_lines) = tb_v1605; }
    { const char * tb_v1606 = "                char fd[6]; for (int i = 5; i >= 0; i--) { fd[i] = (char)('0' + fp % 10); fp /= 10; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1606; }
    { const char * tb_v1607 = "                int last = 5; while (last > 0 && fd[last] == '0') last--;\n"; *(const char * *)tb_grow(v_lines) = tb_v1607; }
    { const char * tb_v1608 = "                for (int i = 0; i <= last; i++) TB_O(fd[i]);\n"; *(const char * *)tb_grow(v_lines) = tb_v1608; }
    { const char * tb_v1609 = "            }\n"; *(const char * *)tb_grow(v_lines) = tb_v1609; }
    { const char * tb_v1610 = "        } else TB_O(*f);\n"; *(const char * *)tb_grow(v_lines) = tb_v1610; }
    { const char * tb_v1611 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1611; }
    { const char * tb_v1612 = "    if (cap) out[n < cap ? n : cap - 1] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1612; }
    { const char * tb_v1613 = "#undef TB_O\n"; *(const char * *)tb_grow(v_lines) = tb_v1613; }
    { const char * tb_v1614 = "    return (int)n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1614; }
    { const char * tb_v1615 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1615; }
    { const char * tb_v1616 = "static int snprintf(char *out, size_t cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = tb_vfmt(out, cap, f, ap); va_end(ap); return n; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1616; }
    { const char * tb_v1617 = "static int printf(const char *f, ...) { char b[512]; va_list ap; va_start(ap, f); int n = tb_vfmt(b, sizeof b, f, ap); va_end(ap); fputs(b, stdout); return n; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1617; }
    { const char * tb_v1618 = "static int fprintf(FILE *o, const char *f, ...) { char b[512]; va_list ap; va_start(ap, f); int n = tb_vfmt(b, sizeof b, f, ap); va_end(ap); fputs(b, o); return n; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1618; }
    { const char * tb_v1619 = "// сортировка вставками (списки в ядре маленькие)\n"; *(const char * *)tb_grow(v_lines) = tb_v1619; }
    { const char * tb_v1620 = "static void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1620; }
    { const char * tb_v1621 = "    unsigned char *a = base, tmp[64];\n"; *(const char * *)tb_grow(v_lines) = tb_v1621; }
    { const char * tb_v1622 = "    if (size > sizeof tmp) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1622; }
    { const char * tb_v1623 = "    for (size_t i = 1; i < n; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1623; }
    { const char * tb_v1624 = "        memcpy(tmp, a + i * size, size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1624; }
    { const char * tb_v1625 = "        size_t j = i;\n"; *(const char * *)tb_grow(v_lines) = tb_v1625; }
    { const char * tb_v1626 = "        while (j > 0 && cmp(a + (j - 1) * size, tmp) > 0) { memcpy(a + j * size, a + (j - 1) * size, size); j--; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1626; }
    { const char * tb_v1627 = "        memcpy(a + j * size, tmp, size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1627; }
    { const char * tb_v1628 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1628; }
    { const char * tb_v1629 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1629; }
    { const char * tb_v1630 = "// файлов, ввода и команд нет\n"; *(const char * *)tb_grow(v_lines) = tb_v1630; }
    { const char * tb_v1631 = "static FILE *fopen(const char *p, const char *m) { (void)p; (void)m; return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1631; }
    { const char * tb_v1632 = "static int fclose(FILE *f) { (void)f; return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1632; }
    { const char * tb_v1633 = "static size_t fread(void *b, size_t s, size_t n, FILE *f) { (void)b; (void)s; (void)n; (void)f; return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1633; }
    { const char * tb_v1634 = "static int getchar(void) { return EOF; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1634; }
    { const char * tb_v1635 = "static int system(const char *c) { (void)c; return -1; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1635; }
    { const char * tb_v1636 = "// одно ядро: «потоки» выполняются сразу, по очереди\n"; *(const char * *)tb_grow(v_lines) = tb_v1636; }
    { const char * tb_v1637 = "static long sysconf(int n) { (void)n; return 1; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1637; }
    { const char * tb_v1638 = "static int pthread_create(pthread_t *t, void *a, void *(*run)(void *), void *arg) { (void)t; (void)a; run(arg); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1638; }
    { const char * tb_v1639 = "static int pthread_join(pthread_t t, void **r) { (void)t; (void)r; return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1639; }
    { const char * tb_v1640 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1640; }
    { const char * tb_v1641 = "// загрузка: заголовок multiboot (его ищут QEMU и GRUB), свой стек, потом tb_kstart\n"; *(const char * *)tb_grow(v_lines) = tb_v1641; }
    { const char * tb_v1642 = "__attribute__((section(\".multiboot\"), used, aligned(4)))\n"; *(const char * *)tb_grow(v_lines) = tb_v1642; }
    { const char * tb_v1643 = "static const unsigned int tb_multiboot[3] = {0x1BADB002u, 0u, (unsigned int)-0x1BADB002};\n"; *(const char * *)tb_grow(v_lines) = tb_v1643; }
    { const char * tb_v1644 = "static unsigned char tb_stack[1 << 20] __attribute__((aligned(16), used));\n"; *(const char * *)tb_grow(v_lines) = tb_v1644; }
    { const char * tb_v1645 = "void tb_kmain(void);\n"; *(const char * *)tb_grow(v_lines) = tb_v1645; }
    { const char * tb_v1646 = "__attribute__((used)) void tb_kstart(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1646; }
    { const char * tb_v1647 = "    __asm__ volatile(\"fninit\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1647; }
    { const char * tb_v1648 = "    // COM1: 115200 бод, 8 бит\n"; *(const char * *)tb_grow(v_lines) = tb_v1648; }
    { const char * tb_v1649 = "    tb_outb(0x3F8 + 1, 0x00); tb_outb(0x3F8 + 3, 0x80); tb_outb(0x3F8 + 0, 0x01);\n"; *(const char * *)tb_grow(v_lines) = tb_v1649; }
    { const char * tb_v1650 = "    tb_outb(0x3F8 + 1, 0x00); tb_outb(0x3F8 + 3, 0x03); tb_outb(0x3F8 + 2, 0xC7);\n"; *(const char * *)tb_grow(v_lines) = tb_v1650; }
    { const char * tb_v1651 = "    for (int i = 0; i < 80 * 25; i++) tb_vga[i] = (unsigned short)(tb_vcolor << 8 | ' ');\n"; *(const char * *)tb_grow(v_lines) = tb_v1651; }
    { const char * tb_v1652 = "    tb_outb(0x3D4, 0x0A); tb_outb(0x3D5, 0x20);  // мигающий курсор не нужен\n"; *(const char * *)tb_grow(v_lines) = tb_v1652; }
    { const char * tb_v1653 = "    tb_kmain();\n"; *(const char * *)tb_grow(v_lines) = tb_v1653; }
    { const char * tb_v1654 = "    exit(0);\n"; *(const char * *)tb_grow(v_lines) = tb_v1654; }
    { const char * tb_v1655 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1655; }
    { const char * tb_v1656 = "__asm__(\".text\\n.global _start\\n_start:\\n  mov $tb_stack + 1048576, %esp\\n  call tb_kstart\\n1: cli\\n  hlt\\n  jmp 1b\\n\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1656; }
    { const char * tb_v1657 = "#else\n"; *(const char * *)tb_grow(v_lines) = tb_v1657; }
    { const char * tb_v1658 = "#include <stdio.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1658; }
    { const char * tb_v1659 = "#include <stdlib.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1659; }
    { const char * tb_v1660 = "#include <string.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1660; }
    { const char * tb_v1661 = "#include <math.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1661; }
    { const char * tb_v1662 = "#include <pthread.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1662; }
    { const char * tb_v1663 = "#include <unistd.h>\n"; *(const char * *)tb_grow(v_lines) = tb_v1663; }
    { const char * tb_v1664 = "#endif\n"; *(const char * *)tb_grow(v_lines) = tb_v1664; }
    { const char * tb_v1665 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1665; }
    { const char * tb_v1666 = "// подсказки для clang / gcc: ошибка бывает редко, а tb_crash не возвращается\n"; *(const char * *)tb_grow(v_lines) = tb_v1666; }
    { const char * tb_v1667 = "#if defined(__GNUC__)\n"; *(const char * *)tb_grow(v_lines) = tb_v1667; }
    { const char * tb_v1668 = "#define TB_RARE(x) __builtin_expect(!!(x), 0)\n"; *(const char * *)tb_grow(v_lines) = tb_v1668; }
    { const char * tb_v1669 = "#define TB_COLD __attribute__((cold, noreturn, noinline))\n"; *(const char * *)tb_grow(v_lines) = tb_v1669; }
    { const char * tb_v1670 = "#else\n"; *(const char * *)tb_grow(v_lines) = tb_v1670; }
    { const char * tb_v1671 = "#define TB_RARE(x) (x)\n"; *(const char * *)tb_grow(v_lines) = tb_v1671; }
    { const char * tb_v1672 = "#define TB_COLD\n"; *(const char * *)tb_grow(v_lines) = tb_v1672; }
    { const char * tb_v1673 = "#endif\n"; *(const char * *)tb_grow(v_lines) = tb_v1673; }
    { const char * tb_v1674 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1674; }
    { const char * tb_v1675 = "// список: кусок памяти, len элементов по size байт, место на cap\n"; *(const char * *)tb_grow(v_lines) = tb_v1675; }
    { const char * tb_v1676 = "typedef struct { char *data; long long len; long long cap; long long size; } tb_list;\n"; *(const char * *)tb_grow(v_lines) = tb_v1676; }
    { const char * tb_v1677 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v1677; }
    { const char * tb_v1678 = "TB_COLD static void tb_crash(int line, const char *msg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1678; }
    { const char * tb_v1679 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v1679; }
    { const char * tb_v1680 = "    // в терминале — красным (если не задан NO_COLOR)\n"; *(const char * *)tb_grow(v_lines) = tb_v1680; }
    { const char * tb_v1681 = "    int color = isatty(2) && getenv(\"NO_COLOR\") == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1681; }
    { const char * tb_v1682 = "    if (color) fputs(\"\\033[31m\", stderr);\n"; *(const char * *)tb_grow(v_lines) = tb_v1682; }
    { const char * tb_v1683 = "    if (line > 0) fprintf(stderr, \"строка %d: %s\", line, msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v1683; }
    { const char * tb_v1684 = "    else fprintf(stderr, \"%s\", msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v1684; }
    { const char * tb_v1685 = "    fputs(color ? \"\\033[0m\\n\" : \"\\n\", stderr);\n"; *(const char * *)tb_grow(v_lines) = tb_v1685; }
    { const char * tb_v1686 = "    exit(1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1686; }
    { const char * tb_v1687 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1687; }
    { const char * tb_v1688 = "// ---------- temp: временная память ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1688; }
    { const char * tb_v1689 = "// Внутри блока temp память берётся из больших кусков подряд (быстро, без malloc),\n"; *(const char * *)tb_grow(v_lines) = tb_v1689; }
    { const char * tb_v1690 = "// а в конце блока все куски отдаются разом. У каждого ядра свои куски.\n"; *(const char * *)tb_grow(v_lines) = tb_v1690; }
    { const char * tb_v1691 = "typedef struct { char *mem; long long size, used; } tb_chunk;\n"; *(const char * *)tb_grow(v_lines) = tb_v1691; }
    { const char * tb_v1692 = "typedef struct { int n; long long used; } tb_mark;\n"; *(const char * *)tb_grow(v_lines) = tb_v1692; }
    { const char * tb_v1693 = "static _Thread_local tb_chunk *tb_chunks = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1693; }
    { const char * tb_v1694 = "static _Thread_local int tb_nchunks = 0, tb_capchunks = 0, tb_temp_depth = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1694; }
    { const char * tb_v1695 = "static int tb_in_temp(const void *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1695; }
    { const char * tb_v1696 = "    for (int i = tb_nchunks - 1; i >= 0; i--) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1696; }
    { const char * tb_v1697 = "        if ((const char *)p >= tb_chunks[i].mem && (const char *)p < tb_chunks[i].mem + tb_chunks[i].size) return 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1697; }
    { const char * tb_v1698 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1698; }
    { const char * tb_v1699 = "    return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1699; }
    { const char * tb_v1700 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1700; }
    { const char * tb_v1701 = "static void *tb_temp_alloc(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1701; }
    { const char * tb_v1702 = "    n = n > 0 ? (n + 15) & ~15LL : 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v1702; }
    { const char * tb_v1703 = "    tb_chunk *c = tb_nchunks > 0 ? &tb_chunks[tb_nchunks - 1] : 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1703; }
    { const char * tb_v1704 = "    if (c == 0 || c->used + n > c->size) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1704; }
    { const char * tb_v1705 = "        long long size = c != 0 ? c->size * 2 : (1LL << 20);\n"; *(const char * *)tb_grow(v_lines) = tb_v1705; }
    { const char * tb_v1706 = "        if (size < n) size = n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1706; }
    { const char * tb_v1707 = "        if (tb_nchunks == tb_capchunks) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1707; }
    { const char * tb_v1708 = "            tb_capchunks = tb_capchunks * 2 + 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1708; }
    { const char * tb_v1709 = "            tb_chunks = realloc(tb_chunks, sizeof(tb_chunk) * tb_capchunks);\n"; *(const char * *)tb_grow(v_lines) = tb_v1709; }
    { const char * tb_v1710 = "            if (tb_chunks == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1710; }
    { const char * tb_v1711 = "        }\n"; *(const char * *)tb_grow(v_lines) = tb_v1711; }
    { const char * tb_v1712 = "        char *mem = malloc(size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1712; }
    { const char * tb_v1713 = "        if (mem == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1713; }
    { const char * tb_v1714 = "        tb_chunks[tb_nchunks] = (tb_chunk){mem, size, 0};\n"; *(const char * *)tb_grow(v_lines) = tb_v1714; }
    { const char * tb_v1715 = "        c = &tb_chunks[tb_nchunks];\n"; *(const char * *)tb_grow(v_lines) = tb_v1715; }
    { const char * tb_v1716 = "        tb_nchunks += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1716; }
    { const char * tb_v1717 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1717; }
    { const char * tb_v1718 = "    void *p = c->mem + c->used;\n"; *(const char * *)tb_grow(v_lines) = tb_v1718; }
    { const char * tb_v1719 = "    c->used += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1719; }
    { const char * tb_v1720 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1720; }
    { const char * tb_v1721 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1721; }
    { const char * tb_v1722 = "// метки открытых temp (и арен шага / функции): самая последняя — текущая область\n"; *(const char * *)tb_grow(v_lines) = tb_v1722; }
    { const char * tb_v1723 = "static _Thread_local tb_mark *tb_marks = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1723; }
    { const char * tb_v1724 = "static _Thread_local int tb_capmarks = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1724; }
    { const char * tb_v1725 = "static inline tb_mark tb_temp_begin(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1725; }
    { const char * tb_v1726 = "    tb_mark m = {tb_nchunks, tb_nchunks > 0 ? tb_chunks[tb_nchunks - 1].used : 0};\n"; *(const char * *)tb_grow(v_lines) = tb_v1726; }
    { const char * tb_v1727 = "    if (tb_temp_depth == tb_capmarks) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1727; }
    { const char * tb_v1728 = "        tb_capmarks = tb_capmarks * 2 + 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v1728; }
    { const char * tb_v1729 = "        tb_marks = realloc(tb_marks, sizeof(tb_mark) * tb_capmarks);\n"; *(const char * *)tb_grow(v_lines) = tb_v1729; }
    { const char * tb_v1730 = "        if (tb_marks == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1730; }
    { const char * tb_v1731 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1731; }
    { const char * tb_v1732 = "    tb_marks[tb_temp_depth] = m;\n"; *(const char * *)tb_grow(v_lines) = tb_v1732; }
    { const char * tb_v1733 = "    tb_temp_depth += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1733; }
    { const char * tb_v1734 = "    return m;\n"; *(const char * *)tb_grow(v_lines) = tb_v1734; }
    { const char * tb_v1735 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1735; }
    { const char * tb_v1736 = "// p во временной памяти текущей (самой внутренней) области, а не во внешней\n"; *(const char * *)tb_grow(v_lines) = tb_v1736; }
    { const char * tb_v1737 = "static int tb_in_cur_temp(const void *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1737; }
    { const char * tb_v1738 = "    if (tb_temp_depth == 0) return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1738; }
    { const char * tb_v1739 = "    tb_mark t = tb_marks[tb_temp_depth - 1];\n"; *(const char * *)tb_grow(v_lines) = tb_v1739; }
    { const char * tb_v1740 = "    for (int i = tb_nchunks - 1; i >= 0; i--) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1740; }
    { const char * tb_v1741 = "        const char *m = tb_chunks[i].mem;\n"; *(const char * *)tb_grow(v_lines) = tb_v1741; }
    { const char * tb_v1742 = "        if ((const char *)p >= m && (const char *)p < m + tb_chunks[i].size)\n"; *(const char * *)tb_grow(v_lines) = tb_v1742; }
    { const char * tb_v1743 = "            return i > t.n - 1 || (i == t.n - 1 && (const char *)p - m >= t.used);\n"; *(const char * *)tb_grow(v_lines) = tb_v1743; }
    { const char * tb_v1744 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1744; }
    { const char * tb_v1745 = "    return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1745; }
    { const char * tb_v1746 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1746; }
    { const char * tb_v1747 = "// длина последних текстов: s[i] в цикле не пересчитывает strlen каждый раз\n"; *(const char * *)tb_grow(v_lines) = tb_v1747; }
    { const char * tb_v1748 = "static _Thread_local const char *tb_lc_p[2];\n"; *(const char * *)tb_grow(v_lines) = tb_v1748; }
    { const char * tb_v1749 = "static _Thread_local long long tb_lc_n[2];\n"; *(const char * *)tb_grow(v_lines) = tb_v1749; }
    { const char * tb_v1750 = "static _Thread_local int tb_lc_next;\n"; *(const char * *)tb_grow(v_lines) = tb_v1750; }
    { const char * tb_v1751 = "static inline void tb_lc_clear(void) { tb_lc_p[0] = 0; tb_lc_p[1] = 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1751; }
    { const char * tb_v1752 = "static inline void tb_temp_end(tb_mark m) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1752; }
    { const char * tb_v1753 = "    // первый кусок оставляем пустым: следующий temp возьмёт его без malloc\n"; *(const char * *)tb_grow(v_lines) = tb_v1753; }
    { const char * tb_v1754 = "    while (tb_nchunks > m.n && tb_nchunks > 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1754; }
    { const char * tb_v1755 = "        tb_nchunks -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1755; }
    { const char * tb_v1756 = "        free(tb_chunks[tb_nchunks].mem);\n"; *(const char * *)tb_grow(v_lines) = tb_v1756; }
    { const char * tb_v1757 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1757; }
    { const char * tb_v1758 = "    if (tb_nchunks > m.n) tb_chunks[0].used = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1758; }
    { const char * tb_v1759 = "    else if (tb_nchunks > 0) tb_chunks[tb_nchunks - 1].used = m.used;\n"; *(const char * *)tb_grow(v_lines) = tb_v1759; }
    { const char * tb_v1760 = "    tb_temp_depth -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1760; }
    { const char * tb_v1761 = "    tb_lc_clear();\n"; *(const char * *)tb_grow(v_lines) = tb_v1761; }
    { const char * tb_v1762 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1762; }
    { const char * tb_v1763 = "static void *tb_mem(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1763; }
    { const char * tb_v1764 = "    if (tb_temp_depth > 0) return tb_temp_alloc(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1764; }
    { const char * tb_v1765 = "    void *p = malloc(n > 0 ? n : 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1765; }
    { const char * tb_v1766 = "    if (p == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1766; }
    { const char * tb_v1767 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1767; }
    { const char * tb_v1768 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1768; }
    { const char * tb_v1769 = "// вырасти кусок: временный текущей области растёт там же, обычный — через realloc.\n"; *(const char * *)tb_grow(v_lines) = tb_v1769; }
    { const char * tb_v1770 = "// Кусок из внешней области (список создан снаружи, растёт внутри temp или шага цикла)\n"; *(const char * *)tb_grow(v_lines) = tb_v1770; }
    { const char * tb_v1771 = "// переезжает в обычную память: внутренняя область скоро отдаётся, а список живёт дольше.\n"; *(const char * *)tb_grow(v_lines) = tb_v1771; }
    { const char * tb_v1772 = "static void *tb_resize(void *p, long long old_n, long long new_n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1772; }
    { const char * tb_v1773 = "    tb_lc_clear();\n"; *(const char * *)tb_grow(v_lines) = tb_v1773; }
    { const char * tb_v1774 = "    if (tb_nchunks > 0 && tb_in_temp(p)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1774; }
    { const char * tb_v1775 = "        void *q = tb_in_cur_temp(p) ? tb_temp_alloc(new_n) : malloc(new_n > 0 ? new_n : 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1775; }
    { const char * tb_v1776 = "        if (q == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1776; }
    { const char * tb_v1777 = "        memcpy(q, p, old_n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1777; }
    { const char * tb_v1778 = "        return q;\n"; *(const char * *)tb_grow(v_lines) = tb_v1778; }
    { const char * tb_v1779 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1779; }
    { const char * tb_v1780 = "    p = realloc(p, new_n > 0 ? new_n : 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1780; }
    { const char * tb_v1781 = "    if (p == 0) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1781; }
    { const char * tb_v1782 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1782; }
    { const char * tb_v1783 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1783; }
    { const char * tb_v1784 = "static tb_list *tb_list_new(long long size) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1784; }
    { const char * tb_v1785 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1785; }
    { const char * tb_v1786 = "    l->len = 0; l->cap = 4; l->size = size; l->data = tb_mem(size * 4);\n"; *(const char * *)tb_grow(v_lines) = tb_v1786; }
    { const char * tb_v1787 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1787; }
    { const char * tb_v1788 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1788; }
    { const char * tb_v1789 = "static tb_list *tb_list_from(long long n, long long size, const void *src) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1789; }
    { const char * tb_v1790 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1790; }
    { const char * tb_v1791 = "    l->len = n; l->cap = n; l->size = size; l->data = tb_mem(size * n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1791; }
    { const char * tb_v1792 = "    memcpy(l->data, src, size * n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1792; }
    { const char * tb_v1793 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1793; }
    { const char * tb_v1794 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1794; }
    { const char * tb_v1795 = "// size передаётся как sizeof(T): после встраивания C знает его заранее\n"; *(const char * *)tb_grow(v_lines) = tb_v1795; }
    { const char * tb_v1796 = "static inline void *tb_at(tb_list *l, long long i, long long size, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1796; }
    { const char * tb_v1797 = "    // одно сравнение вместо двух: отрицательный номер как unsigned становится огромным\n"; *(const char * *)tb_grow(v_lines) = tb_v1797; }
    { const char * tb_v1798 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1798; }
    { const char * tb_v1799 = "    return l->data + i * size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1799; }
    { const char * tb_v1800 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1800; }
    { const char * tb_v1801 = "static inline void *tb_grow(tb_list *l) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1801; }
    { const char * tb_v1802 = "    // cap < 0: это кусок (each piece in data by N) или список, который сейчас режут на куски\n"; *(const char * *)tb_grow(v_lines) = tb_v1802; }
    { const char * tb_v1803 = "    if (TB_RARE(l->len >= l->cap)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1803; }
    { const char * tb_v1804 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1804; }
    { const char * tb_v1805 = "        long long old = l->cap * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1805; }
    { const char * tb_v1806 = "        l->cap = l->cap * 2 + 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v1806; }
    { const char * tb_v1807 = "        // realloc часто растит большой кусок на месте, без копирования\n"; *(const char * *)tb_grow(v_lines) = tb_v1807; }
    { const char * tb_v1808 = "        l->data = tb_resize(l->data, old, l->cap * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1808; }
    { const char * tb_v1809 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1809; }
    { const char * tb_v1810 = "    l->len += 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1810; }
    { const char * tb_v1811 = "    return l->data + (l->len - 1) * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1811; }
    { const char * tb_v1812 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1812; }
    { const char * tb_v1813 = "static void *tb_pop(tb_list *l, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1813; }
    { const char * tb_v1814 = "    if (l->len == 0) tb_crash(line, \".pop() у пустого списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1814; }
    { const char * tb_v1815 = "    l->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1815; }
    { const char * tb_v1816 = "    return l->data + l->len * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1816; }
    { const char * tb_v1817 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1817; }
    { const char * tb_v1818 = "// ---------- list bool: по одному биту на элемент (в 8 раз меньше памяти) ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v1818; }
    { const char * tb_v1819 = "// тот же tb_list: len и cap — в битах, size = 0, data — слова по 64 бита\n"; *(const char * *)tb_grow(v_lines) = tb_v1819; }
    { const char * tb_v1820 = "static tb_list *tb_bits_new(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1820; }
    { const char * tb_v1821 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1821; }
    { const char * tb_v1822 = "    l->len = 0; l->cap = 64; l->size = 0; l->data = tb_mem(8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1822; }
    { const char * tb_v1823 = "    memset(l->data, 0, 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1823; }
    { const char * tb_v1824 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1824; }
    { const char * tb_v1825 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1825; }
    { const char * tb_v1826 = "static tb_list *tb_bits_from(long long n, const unsigned char *src) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1826; }
    { const char * tb_v1827 = "    tb_list *l = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v1827; }
    { const char * tb_v1828 = "    long long words = (n + 63) / 64 + (n == 0);\n"; *(const char * *)tb_grow(v_lines) = tb_v1828; }
    { const char * tb_v1829 = "    l->len = n; l->cap = words * 64; l->size = 0; l->data = tb_mem(words * 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1829; }
    { const char * tb_v1830 = "    memset(l->data, 0, words * 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1830; }
    { const char * tb_v1831 = "    unsigned long long *w = (unsigned long long *)l->data;\n"; *(const char * *)tb_grow(v_lines) = tb_v1831; }
    { const char * tb_v1832 = "    for (long long i = 0; i < n; i++) if (src[i]) w[i >> 6] |= 1ULL << (i & 63);\n"; *(const char * *)tb_grow(v_lines) = tb_v1832; }
    { const char * tb_v1833 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v1833; }
    { const char * tb_v1834 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1834; }
    { const char * tb_v1835 = "// бит без проверки номера (для each — номер всегда внутри)\n"; *(const char * *)tb_grow(v_lines) = tb_v1835; }
    { const char * tb_v1836 = "#define TB_BIT(l, i) ((_Bool)((((unsigned long long *)(l)->data)[(i) >> 6] >> ((i) & 63)) & 1))\n"; *(const char * *)tb_grow(v_lines) = tb_v1836; }
    { const char * tb_v1837 = "static inline _Bool tb_bget(tb_list *l, long long i, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1837; }
    { const char * tb_v1838 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1838; }
    { const char * tb_v1839 = "    return TB_BIT(l, i);\n"; *(const char * *)tb_grow(v_lines) = tb_v1839; }
    { const char * tb_v1840 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1840; }
    { const char * tb_v1841 = "static inline void tb_bset(tb_list *l, long long i, _Bool v, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1841; }
    { const char * tb_v1842 = "    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, \"номер за пределами списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1842; }
    { const char * tb_v1843 = "    unsigned long long *w = (unsigned long long *)l->data + (i >> 6), m = 1ULL << (i & 63);\n"; *(const char * *)tb_grow(v_lines) = tb_v1843; }
    { const char * tb_v1844 = "    *w = v ? (*w | m) : (*w & ~m);\n"; *(const char * *)tb_grow(v_lines) = tb_v1844; }
    { const char * tb_v1845 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1845; }
    { const char * tb_v1846 = "static inline void tb_badd(tb_list *l, _Bool v) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1846; }
    { const char * tb_v1847 = "    if (TB_RARE(l->len >= l->cap)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1847; }
    { const char * tb_v1848 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1848; }
    { const char * tb_v1849 = "        long long old = l->cap / 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v1849; }
    { const char * tb_v1850 = "        l->cap = l->cap * 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1850; }
    { const char * tb_v1851 = "        l->data = tb_resize(l->data, old, l->cap / 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1851; }
    { const char * tb_v1852 = "        memset(l->data + old, 0, l->cap / 8 - old);\n"; *(const char * *)tb_grow(v_lines) = tb_v1852; }
    { const char * tb_v1853 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1853; }
    { const char * tb_v1854 = "    long long i = l->len++;\n"; *(const char * *)tb_grow(v_lines) = tb_v1854; }
    { const char * tb_v1855 = "    unsigned long long *w = (unsigned long long *)l->data + (i >> 6), m = 1ULL << (i & 63);\n"; *(const char * *)tb_grow(v_lines) = tb_v1855; }
    { const char * tb_v1856 = "    *w = v ? (*w | m) : (*w & ~m);\n"; *(const char * *)tb_grow(v_lines) = tb_v1856; }
    { const char * tb_v1857 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1857; }
    { const char * tb_v1858 = "static inline _Bool tb_bpop(tb_list *l, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1858; }
    { const char * tb_v1859 = "    if (l->len == 0) tb_crash(line, \".pop() у пустого списка\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1859; }
    { const char * tb_v1860 = "    l->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1860; }
    { const char * tb_v1861 = "    return TB_BIT(l, l->len);\n"; *(const char * *)tb_grow(v_lines) = tb_v1861; }
    { const char * tb_v1862 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1862; }
    { const char * tb_v1863 = "// N одинаковых битов в конец (repeat N times: flags.add(true))\n"; *(const char * *)tb_grow(v_lines) = tb_v1863; }
    { const char * tb_v1864 = "static void tb_bfill(tb_list *l, long long n, _Bool v) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1864; }
    { const char * tb_v1865 = "    if (n <= 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1865; }
    { const char * tb_v1866 = "    if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1866; }
    { const char * tb_v1867 = "    long long need = l->len + n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1867; }
    { const char * tb_v1868 = "    if (need > l->cap) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1868; }
    { const char * tb_v1869 = "        long long old = l->cap / 8, cap = l->cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1869; }
    { const char * tb_v1870 = "        while (cap < need) cap *= 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v1870; }
    { const char * tb_v1871 = "        l->data = tb_resize(l->data, old, cap / 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v1871; }
    { const char * tb_v1872 = "        memset(l->data + old, 0, cap / 8 - old);\n"; *(const char * *)tb_grow(v_lines) = tb_v1872; }
    { const char * tb_v1873 = "        l->cap = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1873; }
    { const char * tb_v1874 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1874; }
    { const char * tb_v1875 = "    unsigned long long *w = (unsigned long long *)l->data;\n"; *(const char * *)tb_grow(v_lines) = tb_v1875; }
    { const char * tb_v1876 = "    long long i = l->len;\n"; *(const char * *)tb_grow(v_lines) = tb_v1876; }
    { const char * tb_v1877 = "    for (; i < need && (i & 63); i++) w[i >> 6] = v ? (w[i >> 6] | 1ULL << (i & 63)) : (w[i >> 6] & ~(1ULL << (i & 63)));\n"; *(const char * *)tb_grow(v_lines) = tb_v1877; }
    { const char * tb_v1878 = "    if (need - i >= 64) { memset(w + (i >> 6), v ? 0xff : 0, ((need - i) >> 6) * 8); i += (need - i) & ~63LL; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1878; }
    { const char * tb_v1879 = "    for (; i < need; i++) w[i >> 6] = v ? (w[i >> 6] | 1ULL << (i & 63)) : (w[i >> 6] & ~(1ULL << (i & 63)));\n"; *(const char * *)tb_grow(v_lines) = tb_v1879; }
    { const char * tb_v1880 = "    l->len = need;\n"; *(const char * *)tb_grow(v_lines) = tb_v1880; }
    { const char * tb_v1881 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1881; }
    { const char * tb_v1882 = "// значения dict bool лежат байтами — в list bool их надо переложить в биты\n"; *(const char * *)tb_grow(v_lines) = tb_v1882; }
    { const char * tb_v1883 = "static tb_list *tb_bits_of_bytes(tb_list *b) { return tb_bits_from(b->len, (const unsigned char *)b->data); }\n"; *(const char * *)tb_grow(v_lines) = tb_v1883; }
    { const char * tb_v1884 = "// N одинаковых элементов в конец (repeat N times: nums.add(0))\n"; *(const char * *)tb_grow(v_lines) = tb_v1884; }
    { const char * tb_v1885 = "static void tb_fill(tb_list *l, long long n, const void *v) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1885; }
    { const char * tb_v1886 = "    if (n <= 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1886; }
    { const char * tb_v1887 = "    if (l->len + n > l->cap) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1887; }
    { const char * tb_v1888 = "        if (l->cap < 0) tb_crash(0, \"список нельзя растить, пока по нему идёт each ... by N (и сам кусок растить нельзя)\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1888; }
    { const char * tb_v1889 = "        long long cap = l->cap * 2 + 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v1889; }
    { const char * tb_v1890 = "        if (cap < l->len + n) cap = l->len + n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1890; }
    { const char * tb_v1891 = "        l->data = tb_resize(l->data, l->cap * l->size, cap * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1891; }
    { const char * tb_v1892 = "        l->cap = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1892; }
    { const char * tb_v1893 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1893; }
    { const char * tb_v1894 = "    char *p = l->data + l->len * l->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v1894; }
    { const char * tb_v1895 = "    int zero = 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1895; }
    { const char * tb_v1896 = "    for (long long b = 0; b < l->size; b++) if (((const char *)v)[b]) zero = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1896; }
    { const char * tb_v1897 = "    if (zero) memset(p, 0, n * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1897; }
    { const char * tb_v1898 = "    else for (long long i = 0; i < n; i++) memcpy(p + i * l->size, v, l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v1898; }
    { const char * tb_v1899 = "    l->len += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1899; }
    { const char * tb_v1900 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1900; }
    { const char * tb_v1901 = "static void *tb_nn(void *p, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1901; }
    { const char * tb_v1902 = "    if (p == 0) tb_crash(line, \"obj пустой: его ещё не создали\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1902; }
    { const char * tb_v1903 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1903; }
    { const char * tb_v1904 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1904; }
    { const char * tb_v1905 = "static const char *tb_cat(const char *a, const char *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1905; }
    { const char * tb_v1906 = "    size_t la = strlen(a), lb = strlen(b);\n"; *(const char * *)tb_grow(v_lines) = tb_v1906; }
    { const char * tb_v1907 = "    char *s = tb_mem(la + lb + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1907; }
    { const char * tb_v1908 = "    memcpy(s, a, la);\n"; *(const char * *)tb_grow(v_lines) = tb_v1908; }
    { const char * tb_v1909 = "    memcpy(s + la, b, lb + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v1909; }
    { const char * tb_v1910 = "    return s;\n"; *(const char * *)tb_grow(v_lines) = tb_v1910; }
    { const char * tb_v1911 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1911; }
    { const char * tb_v1912 = "static const char *tb_itos(long long x) { char *s = tb_mem(32); snprintf(s, 32, \"%lld\", x); return s; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1912; }
    { const char * tb_v1913 = "static const char *tb_dtos(double x) { char *s = tb_mem(40); snprintf(s, 40, \"%g\", x); return s; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1913; }
    { const char * tb_v1914 = "static const char *tb_btos(int x) { return x ? \"true\" : \"false\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1914; }
    { const char * tb_v1915 = "// текст из одного байта: 256 готовых строк, без выделения памяти на каждую букву\n"; *(const char * *)tb_grow(v_lines) = tb_v1915; }
    { const char * tb_v1916 = "// таблица постоянная: компилятор C видит, что второй байт всегда 0, и  ch == \",\"  становится одним сравнением байта\n"; *(const char * *)tb_grow(v_lines) = tb_v1916; }
    { const char * tb_v1917 = "static const char tb_chars[512] = {\n"; *(const char * *)tb_grow(v_lines) = tb_v1917; }
    { const char * tb_v1918 = "    0,0,1,0,2,0,3,0,4,0,5,0,6,0,7,0,8,0,9,0,10,0,11,0,12,0,13,0,14,0,15,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1918; }
    { const char * tb_v1919 = "    16,0,17,0,18,0,19,0,20,0,21,0,22,0,23,0,24,0,25,0,26,0,27,0,28,0,29,0,30,0,31,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1919; }
    { const char * tb_v1920 = "    32,0,33,0,34,0,35,0,36,0,37,0,38,0,39,0,40,0,41,0,42,0,43,0,44,0,45,0,46,0,47,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1920; }
    { const char * tb_v1921 = "    48,0,49,0,50,0,51,0,52,0,53,0,54,0,55,0,56,0,57,0,58,0,59,0,60,0,61,0,62,0,63,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1921; }
    { const char * tb_v1922 = "    64,0,65,0,66,0,67,0,68,0,69,0,70,0,71,0,72,0,73,0,74,0,75,0,76,0,77,0,78,0,79,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1922; }
    { const char * tb_v1923 = "    80,0,81,0,82,0,83,0,84,0,85,0,86,0,87,0,88,0,89,0,90,0,91,0,92,0,93,0,94,0,95,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1923; }
    { const char * tb_v1924 = "    96,0,97,0,98,0,99,0,100,0,101,0,102,0,103,0,104,0,105,0,106,0,107,0,108,0,109,0,110,0,111,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1924; }
    { const char * tb_v1925 = "    112,0,113,0,114,0,115,0,116,0,117,0,118,0,119,0,120,0,121,0,122,0,123,0,124,0,125,0,126,0,127,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1925; }
    { const char * tb_v1926 = "    128,0,129,0,130,0,131,0,132,0,133,0,134,0,135,0,136,0,137,0,138,0,139,0,140,0,141,0,142,0,143,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1926; }
    { const char * tb_v1927 = "    144,0,145,0,146,0,147,0,148,0,149,0,150,0,151,0,152,0,153,0,154,0,155,0,156,0,157,0,158,0,159,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1927; }
    { const char * tb_v1928 = "    160,0,161,0,162,0,163,0,164,0,165,0,166,0,167,0,168,0,169,0,170,0,171,0,172,0,173,0,174,0,175,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1928; }
    { const char * tb_v1929 = "    176,0,177,0,178,0,179,0,180,0,181,0,182,0,183,0,184,0,185,0,186,0,187,0,188,0,189,0,190,0,191,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1929; }
    { const char * tb_v1930 = "    192,0,193,0,194,0,195,0,196,0,197,0,198,0,199,0,200,0,201,0,202,0,203,0,204,0,205,0,206,0,207,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1930; }
    { const char * tb_v1931 = "    208,0,209,0,210,0,211,0,212,0,213,0,214,0,215,0,216,0,217,0,218,0,219,0,220,0,221,0,222,0,223,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1931; }
    { const char * tb_v1932 = "    224,0,225,0,226,0,227,0,228,0,229,0,230,0,231,0,232,0,233,0,234,0,235,0,236,0,237,0,238,0,239,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1932; }
    { const char * tb_v1933 = "    240,0,241,0,242,0,243,0,244,0,245,0,246,0,247,0,248,0,249,0,250,0,251,0,252,0,253,0,254,0,255,0,\n"; *(const char * *)tb_grow(v_lines) = tb_v1933; }
    { const char * tb_v1934 = "};\n"; *(const char * *)tb_grow(v_lines) = tb_v1934; }
    { const char * tb_v1935 = "static inline const char *tb_chr(long long c) { return tb_chars + 2 * (unsigned char)c; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1935; }
    { const char * tb_v1936 = "static inline long long tb_tlen(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1936; }
    { const char * tb_v1937 = "    if (tb_lc_p[0] == s) return tb_lc_n[0];\n"; *(const char * *)tb_grow(v_lines) = tb_v1937; }
    { const char * tb_v1938 = "    if (tb_lc_p[1] == s) return tb_lc_n[1];\n"; *(const char * *)tb_grow(v_lines) = tb_v1938; }
    { const char * tb_v1939 = "    long long n = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1939; }
    { const char * tb_v1940 = "    tb_lc_p[tb_lc_next] = s; tb_lc_n[tb_lc_next] = n; tb_lc_next ^= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1940; }
    { const char * tb_v1941 = "    return n;\n"; *(const char * *)tb_grow(v_lines) = tb_v1941; }
    { const char * tb_v1942 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1942; }
    { const char * tb_v1943 = "// n — длина, если компилятор её знает (текст, в который дописывают), иначе -1\n"; *(const char * *)tb_grow(v_lines) = tb_v1943; }
    { const char * tb_v1944 = "static inline const char *tb_char_at(const char *s, long long i, long long n, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1944; }
    { const char * tb_v1945 = "    if (n < 0) n = tb_tlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1945; }
    { const char * tb_v1946 = "    if (i < 0 || i >= n) tb_crash(line, \"номер за пределами текста\");\n"; *(const char * *)tb_grow(v_lines) = tb_v1946; }
    { const char * tb_v1947 = "    return tb_chr((unsigned char)s[i]);\n"; *(const char * *)tb_grow(v_lines) = tb_v1947; }
    { const char * tb_v1948 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1948; }
    { const char * tb_v1949 = "// s += b: дописать в конец своей памяти s, а не собирать третий текст из двух.\n"; *(const char * *)tb_grow(v_lines) = tb_v1949; }
    { const char * tb_v1950 = "// *rl — длина s в своей памяти (-1: своей памяти нет, s могли сохранить где-то ещё — сначала копия),\n"; *(const char * *)tb_grow(v_lines) = tb_v1950; }
    { const char * tb_v1951 = "// *rc — сколько там места.\n"; *(const char * *)tb_grow(v_lines) = tb_v1951; }
    { const char * tb_v1952 = "static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1952; }
    { const char * tb_v1953 = "// число цифрами в конец буфера d[24]: вернёт начало цифр, длину — в *n\n"; *(const char * *)tb_grow(v_lines) = tb_v1953; }
    { const char * tb_v1954 = "static inline const char *tb_digits(long long x, char *d, long long *n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1954; }
    { const char * tb_v1955 = "    unsigned long long u = x < 0 ? 0ULL - (unsigned long long)x : (unsigned long long)x;\n"; *(const char * *)tb_grow(v_lines) = tb_v1955; }
    { const char * tb_v1956 = "    char *p = d + 24;\n"; *(const char * *)tb_grow(v_lines) = tb_v1956; }
    { const char * tb_v1957 = "    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);\n"; *(const char * *)tb_grow(v_lines) = tb_v1957; }
    { const char * tb_v1958 = "    if (x < 0) *--p = '-';\n"; *(const char * *)tb_grow(v_lines) = tb_v1958; }
    { const char * tb_v1959 = "    *n = d + 24 - p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1959; }
    { const char * tb_v1960 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1960; }
    { const char * tb_v1961 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1961; }
    { const char * tb_v1962 = "static inline void tb_app(const char **p, long long *rl, long long *rc, const char *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1962; }
    { const char * tb_v1963 = "    tb_app_n(p, rl, rc, b, (long long)strlen(b));\n"; *(const char * *)tb_grow(v_lines) = tb_v1963; }
    { const char * tb_v1964 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1964; }
    { const char * tb_v1965 = "// дописать число: цифры пишутся прямо в конец, без отдельного текста\n"; *(const char * *)tb_grow(v_lines) = tb_v1965; }
    { const char * tb_v1966 = "static inline void tb_app_i(const char **p, long long *rl, long long *rc, long long x) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1966; }
    { const char * tb_v1967 = "    char d[24]; int n = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1967; }
    { const char * tb_v1968 = "    unsigned long long u = x < 0 ? 0ULL - (unsigned long long)x : (unsigned long long)x;\n"; *(const char * *)tb_grow(v_lines) = tb_v1968; }
    { const char * tb_v1969 = "    do { d[23 - n++] = (char)('0' + u % 10); u /= 10; } while (u);\n"; *(const char * *)tb_grow(v_lines) = tb_v1969; }
    { const char * tb_v1970 = "    if (x < 0) d[23 - n++] = '-';\n"; *(const char * *)tb_grow(v_lines) = tb_v1970; }
    { const char * tb_v1971 = "    tb_app_n(p, rl, rc, d + 24 - n, n);\n"; *(const char * *)tb_grow(v_lines) = tb_v1971; }
    { const char * tb_v1972 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1972; }
    { const char * tb_v1973 = "static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1973; }
    { const char * tb_v1974 = "// ядро отдаёт свой кусок в общий текст: если общий ещё пустой — забирает память куска целиком, без копии\n"; *(const char * *)tb_grow(v_lines) = tb_v1974; }
    { const char * tb_v1975 = "static void tb_app_take(const char **p, long long *rl, long long *rc, const char *b, long long brl, long long brc) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1975; }
    { const char * tb_v1976 = "    if (*rl < 0 && (*p)[0] == 0 && brl >= 0) { *p = b; *rl = brl; *rc = brc; return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v1976; }
    { const char * tb_v1977 = "    tb_app_n(p, rl, rc, b, brl >= 0 ? brl : (long long)strlen(b));\n"; *(const char * *)tb_grow(v_lines) = tb_v1977; }
    { const char * tb_v1978 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v1978; }
    { const char * tb_v1979 = "static void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1979; }
    { const char * tb_v1980 = "    char *s = (char *)*p;\n"; *(const char * *)tb_grow(v_lines) = tb_v1980; }
    { const char * tb_v1981 = "    if (*rl < 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1981; }
    { const char * tb_v1982 = "        long long la = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v1982; }
    { const char * tb_v1983 = "        long long cap = 2 * (la + lb) + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1983; }
    { const char * tb_v1984 = "        if (cap < 64) cap = 64;\n"; *(const char * *)tb_grow(v_lines) = tb_v1984; }
    { const char * tb_v1985 = "        char *r = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1985; }
    { const char * tb_v1986 = "        memcpy(r, s, la);\n"; *(const char * *)tb_grow(v_lines) = tb_v1986; }
    { const char * tb_v1987 = "        memcpy(r + la, b, lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v1987; }
    { const char * tb_v1988 = "        r[la + lb] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v1988; }
    { const char * tb_v1989 = "        *p = r; *rl = la + lb; *rc = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1989; }
    { const char * tb_v1990 = "        return;\n"; *(const char * *)tb_grow(v_lines) = tb_v1990; }
    { const char * tb_v1991 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1991; }
    { const char * tb_v1992 = "    long long need = *rl + lb + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1992; }
    { const char * tb_v1993 = "    if (need > *rc) {\n"; *(const char * *)tb_grow(v_lines) = tb_v1993; }
    { const char * tb_v1994 = "        long long cap = 2 * need;\n"; *(const char * *)tb_grow(v_lines) = tb_v1994; }
    { const char * tb_v1995 = "        long long off = (b >= s && b < s + *rc) ? b - s : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v1995; }
    { const char * tb_v1996 = "        s = tb_resize(s, *rl + 1, cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v1996; }
    { const char * tb_v1997 = "        if (off >= 0) b = s + off;\n"; *(const char * *)tb_grow(v_lines) = tb_v1997; }
    { const char * tb_v1998 = "        *p = s; *rc = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v1998; }
    { const char * tb_v1999 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v1999; }
    { const char * tb_v2000 = "    memmove(s + *rl, b, lb);\n"; *(const char * *)tb_grow(v_lines) = tb_v2000; }
    { const char * tb_v2001 = "    *rl += lb;\n"; *(const char * *)tb_grow(v_lines) = tb_v2001; }
    { const char * tb_v2002 = "    s[*rl] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2002; }
    { const char * tb_v2003 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2003; }
    { const char * tb_v2004 = "static const char *tb_cut(const char *s, long long from, long long to, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2004; }
    { const char * tb_v2005 = "    long long n = (long long)strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v2005; }
    { const char * tb_v2006 = "    if (from < 0 || to >= n || from > to + 1) tb_crash(line, \"cut: номера за пределами текста\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2006; }
    { const char * tb_v2007 = "    char *r = tb_mem(to - from + 2);\n"; *(const char * *)tb_grow(v_lines) = tb_v2007; }
    { const char * tb_v2008 = "    memcpy(r, s + from, to - from + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v2008; }
    { const char * tb_v2009 = "    r[to - from + 1] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2009; }
    { const char * tb_v2010 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v2010; }
    { const char * tb_v2011 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2011; }
    { const char * tb_v2012 = "static long long tb_code(const char *s, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2012; }
    { const char * tb_v2013 = "    if (s[0] == 0) tb_crash(line, \"code: текст пустой\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2013; }
    { const char * tb_v2014 = "    return (unsigned char)s[0];\n"; *(const char * *)tb_grow(v_lines) = tb_v2014; }
    { const char * tb_v2015 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2015; }
    { const char * tb_v2016 = "static long long tb_div(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"деление на ноль\"); return a / b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2016; }
    { const char * tb_v2017 = "static long long tb_mod(long long a, long long b, int line) { if (b == 0) tb_crash(line, \"остаток от деления на ноль\"); return a % b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2017; }
    { const char * tb_v2018 = "static long long tb_round(double x) { return (long long)(x < 0 ? x - 0.5 : x + 0.5); }\n"; *(const char * *)tb_grow(v_lines) = tb_v2018; }
    { const char * tb_v2019 = "static void tb_ps(const char *s) { fputs(s, stdout); }\n"; *(const char * *)tb_grow(v_lines) = tb_v2019; }
    { const char * tb_v2020 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2020; }
    { const char * tb_v2021 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2021; }
    { const char * tb_v2022 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2022; }
    { const char * tb_v2023 = "// ---------- ошибки fail / try / or / else err ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2023; }
    { const char * tb_v2024 = "// Функция, которая сломалась, кладёт текст ошибки в tb_err и сразу выходит.\n"; *(const char * *)tb_grow(v_lines) = tb_v2024; }
    { const char * tb_v2025 = "// Тот, кто её вызвал, смотрит в tb_err.\n"; *(const char * *)tb_grow(v_lines) = tb_v2025; }
    { const char * tb_v2026 = "static _Thread_local const char *tb_err = 0;   // у каждого ядра своя\n"; *(const char * *)tb_grow(v_lines) = tb_v2026; }
    { const char * tb_v2027 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2027; }
    { const char * tb_v2028 = "TB_COLD static void tb_die(const char *msg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2028; }
    { const char * tb_v2029 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v2029; }
    { const char * tb_v2030 = "    fprintf(stderr, \"ошибка: %s\\n\", msg);\n"; *(const char * *)tb_grow(v_lines) = tb_v2030; }
    { const char * tb_v2031 = "    exit(1);\n"; *(const char * *)tb_grow(v_lines) = tb_v2031; }
    { const char * tb_v2032 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2032; }
    { const char * tb_v2033 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2033; }
    { const char * tb_v2034 = "// ---------- текст ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2034; }
    { const char * tb_v2035 = "static const char *tb_upper(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2035; }
    { const char * tb_v2036 = "    size_t n = strlen(s); char *r = tb_mem(n + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v2036; }
    { const char * tb_v2037 = "    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v2037; }
    { const char * tb_v2038 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v2038; }
    { const char * tb_v2039 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2039; }
    { const char * tb_v2040 = "static const char *tb_lower(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2040; }
    { const char * tb_v2041 = "    size_t n = strlen(s); char *r = tb_mem(n + 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v2041; }
    { const char * tb_v2042 = "    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v2042; }
    { const char * tb_v2043 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v2043; }
    { const char * tb_v2044 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2044; }
    { const char * tb_v2045 = "static int tb_space(char c) { return c == ' ' || c == '\\t' || c == '\\n' || c == '\\r'; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2045; }
    { const char * tb_v2046 = "static const char *tb_trim(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2046; }
    { const char * tb_v2047 = "    size_t a = 0, b = strlen(s);\n"; *(const char * *)tb_grow(v_lines) = tb_v2047; }
    { const char * tb_v2048 = "    while (a < b && tb_space(s[a])) a++;\n"; *(const char * *)tb_grow(v_lines) = tb_v2048; }
    { const char * tb_v2049 = "    while (b > a && tb_space(s[b - 1])) b--;\n"; *(const char * *)tb_grow(v_lines) = tb_v2049; }
    { const char * tb_v2050 = "    char *r = tb_mem(b - a + 1); memcpy(r, s + a, b - a); r[b - a] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2050; }
    { const char * tb_v2051 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v2051; }
    { const char * tb_v2052 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2052; }
    { const char * tb_v2053 = "static long long tb_find(const char *s, const char *part) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2053; }
    { const char * tb_v2054 = "    const char *p = strstr(s, part);\n"; *(const char * *)tb_grow(v_lines) = tb_v2054; }
    { const char * tb_v2055 = "    return p ? (long long)(p - s) : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2055; }
    { const char * tb_v2056 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2056; }
    { const char * tb_v2057 = "static int tb_starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2057; }
    { const char * tb_v2058 = "static int tb_ends(const char *s, const char *p) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2058; }
    { const char * tb_v2059 = "    size_t a = strlen(s), b = strlen(p);\n"; *(const char * *)tb_grow(v_lines) = tb_v2059; }
    { const char * tb_v2060 = "    return b <= a && strcmp(s + a - b, p) == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2060; }
    { const char * tb_v2061 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2061; }
    { const char * tb_v2062 = "static const char *tb_replace(const char *s, const char *from, const char *to, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2062; }
    { const char * tb_v2063 = "    size_t lf = strlen(from), lt = strlen(to), count = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2063; }
    { const char * tb_v2064 = "    if (lf == 0) tb_crash(line, \"replace: что заменять — пустой текст\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2064; }
    { const char * tb_v2065 = "    for (const char *p = strstr(s, from); p; p = strstr(p + lf, from)) count++;\n"; *(const char * *)tb_grow(v_lines) = tb_v2065; }
    { const char * tb_v2066 = "    char *r = tb_mem(strlen(s) + count * lt + 1), *w = r;\n"; *(const char * *)tb_grow(v_lines) = tb_v2066; }
    { const char * tb_v2067 = "    const char *p = s, *q;\n"; *(const char * *)tb_grow(v_lines) = tb_v2067; }
    { const char * tb_v2068 = "    while ((q = strstr(p, from)) != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2068; }
    { const char * tb_v2069 = "        memcpy(w, p, q - p); w += q - p;\n"; *(const char * *)tb_grow(v_lines) = tb_v2069; }
    { const char * tb_v2070 = "        memcpy(w, to, lt); w += lt;\n"; *(const char * *)tb_grow(v_lines) = tb_v2070; }
    { const char * tb_v2071 = "        p = q + lf;\n"; *(const char * *)tb_grow(v_lines) = tb_v2071; }
    { const char * tb_v2072 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2072; }
    { const char * tb_v2073 = "    strcpy(w, p);\n"; *(const char * *)tb_grow(v_lines) = tb_v2073; }
    { const char * tb_v2074 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v2074; }
    { const char * tb_v2075 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2075; }
    { const char * tb_v2076 = "static tb_list *tb_split(const char *s, const char *sep, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2076; }
    { const char * tb_v2077 = "    size_t ls = strlen(sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v2077; }
    { const char * tb_v2078 = "    if (ls == 0) tb_crash(line, \"split: разделитель — пустой текст\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2078; }
    { const char * tb_v2079 = "    tb_list *l = tb_list_new(sizeof(const char *));\n"; *(const char * *)tb_grow(v_lines) = tb_v2079; }
    { const char * tb_v2080 = "    const char *p = s, *q;\n"; *(const char * *)tb_grow(v_lines) = tb_v2080; }
    { const char * tb_v2081 = "    while (1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2081; }
    { const char * tb_v2082 = "        q = strstr(p, sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v2082; }
    { const char * tb_v2083 = "        size_t n = q ? (size_t)(q - p) : strlen(p);\n"; *(const char * *)tb_grow(v_lines) = tb_v2083; }
    { const char * tb_v2084 = "        char *part = tb_mem(n + 1); memcpy(part, p, n); part[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2084; }
    { const char * tb_v2085 = "        *(const char **)tb_grow(l) = part;\n"; *(const char * *)tb_grow(v_lines) = tb_v2085; }
    { const char * tb_v2086 = "        if (!q) break;\n"; *(const char * *)tb_grow(v_lines) = tb_v2086; }
    { const char * tb_v2087 = "        p = q + ls;\n"; *(const char * *)tb_grow(v_lines) = tb_v2087; }
    { const char * tb_v2088 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2088; }
    { const char * tb_v2089 = "    return l;\n"; *(const char * *)tb_grow(v_lines) = tb_v2089; }
    { const char * tb_v2090 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2090; }
    { const char * tb_v2091 = "static const char *tb_join(tb_list *l, const char *sep) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2091; }
    { const char * tb_v2092 = "    size_t n = 0, ls = strlen(sep);\n"; *(const char * *)tb_grow(v_lines) = tb_v2092; }
    { const char * tb_v2093 = "    for (long long i = 0; i < l->len; i++) n += strlen(((const char **)l->data)[i]) + ls;\n"; *(const char * *)tb_grow(v_lines) = tb_v2093; }
    { const char * tb_v2094 = "    char *r = tb_mem(n + 1), *w = r;\n"; *(const char * *)tb_grow(v_lines) = tb_v2094; }
    { const char * tb_v2095 = "    for (long long i = 0; i < l->len; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2095; }
    { const char * tb_v2096 = "        const char *part = ((const char **)l->data)[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v2096; }
    { const char * tb_v2097 = "        if (i > 0) { memcpy(w, sep, ls); w += ls; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2097; }
    { const char * tb_v2098 = "        memcpy(w, part, strlen(part)); w += strlen(part);\n"; *(const char * *)tb_grow(v_lines) = tb_v2098; }
    { const char * tb_v2099 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2099; }
    { const char * tb_v2100 = "    *w = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2100; }
    { const char * tb_v2101 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v2101; }
    { const char * tb_v2102 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2102; }
    { const char * tb_v2103 = "static long long tb_to_int(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2103; }
    { const char * tb_v2104 = "    long long v = 0; int i = 0, minus = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2104; }
    { const char * tb_v2105 = "    if (s[0] == '-') { minus = 1; i = 1; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2105; }
    { const char * tb_v2106 = "    if (s[i] == 0) { tb_err = tb_cat(\"это не число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2106; }
    { const char * tb_v2107 = "    for (; s[i]; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2107; }
    { const char * tb_v2108 = "        if (s[i] < '0' || s[i] > '9') { tb_err = tb_cat(\"это не число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2108; }
    { const char * tb_v2109 = "        v = v * 10 + (s[i] - '0');\n"; *(const char * *)tb_grow(v_lines) = tb_v2109; }
    { const char * tb_v2110 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2110; }
    { const char * tb_v2111 = "    return minus ? -v : v;\n"; *(const char * *)tb_grow(v_lines) = tb_v2111; }
    { const char * tb_v2112 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2112; }
    { const char * tb_v2113 = "static double tb_to_dec(const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2113; }
    { const char * tb_v2114 = "    char *end = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2114; }
    { const char * tb_v2115 = "    double v = strtod(s, &end);\n"; *(const char * *)tb_grow(v_lines) = tb_v2115; }
    { const char * tb_v2116 = "    if (s[0] == 0 || *end != 0) { tb_err = tb_cat(\"это не дробное число: \", s); return 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2116; }
    { const char * tb_v2117 = "    return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v2117; }
    { const char * tb_v2118 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2118; }
    { const char * tb_v2119 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2119; }
    { const char * tb_v2120 = "// ---------- файлы, ввод, слова запуска ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2120; }
    { const char * tb_v2121 = "static const char *tb_read_file(const char *path) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2121; }
    { const char * tb_v2122 = "    FILE *f = fopen(path, \"rb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2122; }
    { const char * tb_v2123 = "    if (f == 0) { tb_err = tb_cat(\"не могу открыть файл: \", path); return \"\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2123; }
    { const char * tb_v2124 = "    size_t cap = 4096, n = 0, got;\n"; *(const char * *)tb_grow(v_lines) = tb_v2124; }
    { const char * tb_v2125 = "    char *buf = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v2125; }
    { const char * tb_v2126 = "    while ((got = fread(buf + n, 1, cap - n - 1, f)) > 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2126; }
    { const char * tb_v2127 = "        n += got;\n"; *(const char * *)tb_grow(v_lines) = tb_v2127; }
    { const char * tb_v2128 = "        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2128; }
    { const char * tb_v2129 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2129; }
    { const char * tb_v2130 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v2130; }
    { const char * tb_v2131 = "    buf[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2131; }
    { const char * tb_v2132 = "    return buf;\n"; *(const char * *)tb_grow(v_lines) = tb_v2132; }
    { const char * tb_v2133 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2133; }
    { const char * tb_v2134 = "static void tb_write_file(const char *path, const char *s) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2134; }
    { const char * tb_v2135 = "    FILE *f = fopen(path, \"wb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2135; }
    { const char * tb_v2136 = "    if (f == 0) { tb_err = tb_cat(\"не могу записать файл: \", path); return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2136; }
    { const char * tb_v2137 = "    fputs(s, f);\n"; *(const char * *)tb_grow(v_lines) = tb_v2137; }
    { const char * tb_v2138 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v2138; }
    { const char * tb_v2139 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2139; }
    { const char * tb_v2140 = "static const char *tb_input(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2140; }
    { const char * tb_v2141 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v2141; }
    { const char * tb_v2142 = "    size_t cap = 256, n = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2142; }
    { const char * tb_v2143 = "    char *buf = tb_mem(cap);\n"; *(const char * *)tb_grow(v_lines) = tb_v2143; }
    { const char * tb_v2144 = "    int c;\n"; *(const char * *)tb_grow(v_lines) = tb_v2144; }
    { const char * tb_v2145 = "    while ((c = getchar()) != EOF && c != '\\n') {\n"; *(const char * *)tb_grow(v_lines) = tb_v2145; }
    { const char * tb_v2146 = "        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2146; }
    { const char * tb_v2147 = "        buf[n++] = (char)c;\n"; *(const char * *)tb_grow(v_lines) = tb_v2147; }
    { const char * tb_v2148 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2148; }
    { const char * tb_v2149 = "    if (c == EOF && n == 0) { tb_err = \"ввод кончился\"; return \"\"; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2149; }
    { const char * tb_v2150 = "    if (n > 0 && buf[n - 1] == '\\r') n--;\n"; *(const char * *)tb_grow(v_lines) = tb_v2150; }
    { const char * tb_v2151 = "    buf[n] = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2151; }
    { const char * tb_v2152 = "    return buf;\n"; *(const char * *)tb_grow(v_lines) = tb_v2152; }
    { const char * tb_v2153 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2153; }
    { const char * tb_v2154 = "static tb_list *tb_args;\n"; *(const char * *)tb_grow(v_lines) = tb_v2154; }
    { const char * tb_v2155 = "static long long tb_run(const char *command) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2155; }
    { const char * tb_v2156 = "    fflush(stdout);\n"; *(const char * *)tb_grow(v_lines) = tb_v2156; }
    { const char * tb_v2157 = "    int code = system(command);\n"; *(const char * *)tb_grow(v_lines) = tb_v2157; }
    { const char * tb_v2158 = "    if (code == -1) return -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2158; }
    { const char * tb_v2159 = "    return code == 0 ? 0 : (code > 255 ? code / 256 : code);\n"; *(const char * *)tb_grow(v_lines) = tb_v2159; }
    { const char * tb_v2160 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2160; }
    { const char * tb_v2161 = "static void tb_exit(long long code) { fflush(stdout); exit((int)code); }\n"; *(const char * *)tb_grow(v_lines) = tb_v2161; }
    { const char * tb_v2162 = "static int tb_file_exists(const char *path) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2162; }
    { const char * tb_v2163 = "    FILE *f = fopen(path, \"rb\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2163; }
    { const char * tb_v2164 = "    if (f == 0) return 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2164; }
    { const char * tb_v2165 = "    fclose(f);\n"; *(const char * *)tb_grow(v_lines) = tb_v2165; }
    { const char * tb_v2166 = "    return 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2166; }
    { const char * tb_v2167 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2167; }
    { const char * tb_v2168 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2168; }
    { const char * tb_v2169 = "// ---------- числа ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2169; }
    { const char * tb_v2170 = "static long long tb_absi(long long x) { return x < 0 ? -x : x; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2170; }
    { const char * tb_v2171 = "static long long tb_mini(long long a, long long b) { return a < b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2171; }
    { const char * tb_v2172 = "static long long tb_maxi(long long a, long long b) { return a > b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2172; }
    { const char * tb_v2173 = "static double tb_mind(double a, double b) { return a < b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2173; }
    { const char * tb_v2174 = "static double tb_maxd(double a, double b) { return a > b ? a : b; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2174; }
    { const char * tb_v2175 = "static double tb_sqrt(double x, int line) { if (x < 0) tb_crash(line, \"sqrt: корень из отрицательного числа\"); return sqrt(x); }\n"; *(const char * *)tb_grow(v_lines) = tb_v2175; }
    { const char * tb_v2176 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2176; }
    { const char * tb_v2177 = "// ---------- sort ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2177; }
    { const char * tb_v2178 = "static int tb_cmp_int(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return (x > y) - (x < y); }\n"; *(const char * *)tb_grow(v_lines) = tb_v2178; }
    { const char * tb_v2179 = "static int tb_cmp_dec(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }\n"; *(const char * *)tb_grow(v_lines) = tb_v2179; }
    { const char * tb_v2180 = "static int tb_cmp_text(const void *a, const void *b) { return strcmp(*(const char * const *)a, *(const char * const *)b); }\n"; *(const char * *)tb_grow(v_lines) = tb_v2180; }
    { const char * tb_v2181 = "static tb_list *tb_copy(tb_list *l) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2181; }
    { const char * tb_v2182 = "    if (l->size == 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2182; }
    { const char * tb_v2183 = "        tb_list *r = tb_mem(sizeof(tb_list));\n"; *(const char * *)tb_grow(v_lines) = tb_v2183; }
    { const char * tb_v2184 = "        long long bytes = l->cap / 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v2184; }
    { const char * tb_v2185 = "        r->len = l->len; r->cap = l->cap; r->size = 0; r->data = tb_mem(bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v2185; }
    { const char * tb_v2186 = "        memcpy(r->data, l->data, bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v2186; }
    { const char * tb_v2187 = "        return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v2187; }
    { const char * tb_v2188 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2188; }
    { const char * tb_v2189 = "    return tb_list_from(l->len, l->size, l->data);\n"; *(const char * *)tb_grow(v_lines) = tb_v2189; }
    { const char * tb_v2190 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2190; }
    { const char * tb_v2191 = "static void tb_sort_list(tb_list *l, int (*cmp)(const void *, const void *)) { qsort(l->data, l->len, l->size, cmp); }\n"; *(const char * *)tb_grow(v_lines) = tb_v2191; }
    { const char * tb_v2192 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2192; }
    { const char * tb_v2193 = "// ---------- dict: ключи и значения лежат в двух списках по порядку добавления,\n"; *(const char * *)tb_grow(v_lines) = tb_v2193; }
    { const char * tb_v2194 = "// а найти номер по ключу можно тремя способами (mode):\n"; *(const char * *)tb_grow(v_lines) = tb_v2194; }
    { const char * tb_v2195 = "//   1 — «номер = адрес»: ключи int шли подряд base, base+1, base+2 ... — номер = ключ - base, без поиска;\n"; *(const char * *)tb_grow(v_lines) = tb_v2195; }
    { const char * tb_v2196 = "//   2 — «адресное хранение»: ключи int лежат плотно — pos[ключ - base] = номер + 1, один взгляд;\n"; *(const char * *)tb_grow(v_lines) = tb_v2196; }
    { const char * tb_v2197 = "//   0 — таблица slots с хешем (текст или разбросанные числа) ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2197; }
    { const char * tb_v2198 = "typedef struct {\n"; *(const char * *)tb_grow(v_lines) = tb_v2198; }
    { const char * tb_v2199 = "    tb_list *keys; tb_list *vals; long long *slots; long long nslots; int text;\n"; *(const char * *)tb_grow(v_lines) = tb_v2199; }
    { const char * tb_v2200 = "    int mode; long long base; long long *pos; long long npos;\n"; *(const char * *)tb_grow(v_lines) = tb_v2200; }
    { const char * tb_v2201 = "} tb_dict;\n"; *(const char * *)tb_grow(v_lines) = tb_v2201; }
    { const char * tb_v2202 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2202; }
    { const char * tb_v2203 = "static unsigned long long tb_hash(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2203; }
    { const char * tb_v2204 = "    unsigned long long h = 1469598103934665603ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v2204; }
    { const char * tb_v2205 = "    if (d->text) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2205; }
    { const char * tb_v2206 = "        for (const unsigned char *p = *(const unsigned char * const *)k; *p; p++) { h ^= *p; h *= 1099511628211ULL; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2206; }
    { const char * tb_v2207 = "    } else {\n"; *(const char * *)tb_grow(v_lines) = tb_v2207; }
    { const char * tb_v2208 = "        h ^= (unsigned long long)*(const long long *)k;\n"; *(const char * *)tb_grow(v_lines) = tb_v2208; }
    { const char * tb_v2209 = "        h *= 0x9E3779B97F4A7C15ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v2209; }
    { const char * tb_v2210 = "        h ^= h >> 32;\n"; *(const char * *)tb_grow(v_lines) = tb_v2210; }
    { const char * tb_v2211 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2211; }
    { const char * tb_v2212 = "    return h;\n"; *(const char * *)tb_grow(v_lines) = tb_v2212; }
    { const char * tb_v2213 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2213; }
    { const char * tb_v2214 = "static int tb_keq(tb_dict *d, const void *a, const void *b) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2214; }
    { const char * tb_v2215 = "    if (d->text) return strcmp(*(const char * const *)a, *(const char * const *)b) == 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2215; }
    { const char * tb_v2216 = "    return *(const long long *)a == *(const long long *)b;\n"; *(const char * *)tb_grow(v_lines) = tb_v2216; }
    { const char * tb_v2217 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2217; }
    { const char * tb_v2218 = "static inline long long tb_dfind(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2218; }
    { const char * tb_v2219 = "    if (d->mode == 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2219; }
    { const char * tb_v2220 = "        unsigned long long at = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v2220; }
    { const char * tb_v2221 = "        return at < (unsigned long long)d->keys->len ? (long long)at : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2221; }
    { const char * tb_v2222 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2222; }
    { const char * tb_v2223 = "    if (d->mode == 2) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2223; }
    { const char * tb_v2224 = "        unsigned long long o = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v2224; }
    { const char * tb_v2225 = "        return o < (unsigned long long)d->npos ? d->pos[o] - 1 : -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2225; }
    { const char * tb_v2226 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2226; }
    { const char * tb_v2227 = "    unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v2227; }
    { const char * tb_v2228 = "    while (d->slots[i] != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2228; }
    { const char * tb_v2229 = "        long long at = d->slots[i] - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2229; }
    { const char * tb_v2230 = "        if (tb_keq(d, d->keys->data + at * d->keys->size, k)) return at;\n"; *(const char * *)tb_grow(v_lines) = tb_v2230; }
    { const char * tb_v2231 = "        i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v2231; }
    { const char * tb_v2232 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2232; }
    { const char * tb_v2233 = "    return -1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2233; }
    { const char * tb_v2234 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2234; }
    { const char * tb_v2235 = "// память для таблиц dict — там же, где лежат его ключи (временная или обычная)\n"; *(const char * *)tb_grow(v_lines) = tb_v2235; }
    { const char * tb_v2236 = "static void *tb_dalloc(tb_dict *d, void *old, long long bytes) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2236; }
    { const char * tb_v2237 = "    if (old && !(tb_nchunks > 0 && tb_in_temp(old))) free(old);\n"; *(const char * *)tb_grow(v_lines) = tb_v2237; }
    { const char * tb_v2238 = "    void *p = tb_in_cur_temp(d->keys->data) ? tb_temp_alloc(bytes) : malloc(bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v2238; }
    { const char * tb_v2239 = "    if (!p) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2239; }
    { const char * tb_v2240 = "    return p;\n"; *(const char * *)tb_grow(v_lines) = tb_v2240; }
    { const char * tb_v2241 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2241; }
    { const char * tb_v2242 = "static void tb_dreindex(tb_dict *d, long long nslots) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2242; }
    { const char * tb_v2243 = "    d->mode = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2243; }
    { const char * tb_v2244 = "    if (d->pos) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2244; }
    { const char * tb_v2245 = "        if (!(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);\n"; *(const char * *)tb_grow(v_lines) = tb_v2245; }
    { const char * tb_v2246 = "        d->pos = 0; d->npos = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2246; }
    { const char * tb_v2247 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2247; }
    { const char * tb_v2248 = "    d->nslots = nslots;\n"; *(const char * *)tb_grow(v_lines) = tb_v2248; }
    { const char * tb_v2249 = "    d->slots = tb_dalloc(d, d->slots, nslots * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v2249; }
    { const char * tb_v2250 = "    memset(d->slots, 0, nslots * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v2250; }
    { const char * tb_v2251 = "    unsigned long long mask = nslots - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2251; }
    { const char * tb_v2252 = "    for (long long at = 0; at < d->keys->len; at++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2252; }
    { const char * tb_v2253 = "        unsigned long long i = tb_hash(d, d->keys->data + at * d->keys->size) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v2253; }
    { const char * tb_v2254 = "        while (d->slots[i] != 0) i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v2254; }
    { const char * tb_v2255 = "        d->slots[i] = at + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2255; }
    { const char * tb_v2256 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2256; }
    { const char * tb_v2257 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2257; }
    { const char * tb_v2258 = "// перейти на таблицу с хешем (ключ ушёл далеко от остальных или его удалили)\n"; *(const char * *)tb_grow(v_lines) = tb_v2258; }
    { const char * tb_v2259 = "static void tb_dto_hash(tb_dict *d) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2259; }
    { const char * tb_v2260 = "    long long n = 8;\n"; *(const char * *)tb_grow(v_lines) = tb_v2260; }
    { const char * tb_v2261 = "    while (n < d->keys->len * 2 + 2) n *= 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v2261; }
    { const char * tb_v2262 = "    tb_dreindex(d, n);\n"; *(const char * *)tb_grow(v_lines) = tb_v2262; }
    { const char * tb_v2263 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2263; }
    { const char * tb_v2264 = "// mode 2: таблица pos на ключи base .. base+npos-1\n"; *(const char * *)tb_grow(v_lines) = tb_v2264; }
    { const char * tb_v2265 = "static void tb_dto_pos(tb_dict *d, long long npos) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2265; }
    { const char * tb_v2266 = "    long long *p = tb_dalloc(d, 0, npos * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v2266; }
    { const char * tb_v2267 = "    memset(p, 0, npos * sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v2267; }
    { const char * tb_v2268 = "    const long long *ks = (const long long *)d->keys->data;\n"; *(const char * *)tb_grow(v_lines) = tb_v2268; }
    { const char * tb_v2269 = "    for (long long at = 0; at < d->keys->len; at++) p[ks[at] - d->base] = at + 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2269; }
    { const char * tb_v2270 = "    if (d->pos && !(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);\n"; *(const char * *)tb_grow(v_lines) = tb_v2270; }
    { const char * tb_v2271 = "    d->pos = p;\n"; *(const char * *)tb_grow(v_lines) = tb_v2271; }
    { const char * tb_v2272 = "    d->npos = npos;\n"; *(const char * *)tb_grow(v_lines) = tb_v2272; }
    { const char * tb_v2273 = "    d->mode = 2;\n"; *(const char * *)tb_grow(v_lines) = tb_v2273; }
    { const char * tb_v2274 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2274; }
    { const char * tb_v2275 = "static tb_dict *tb_dict_new(int text, long long vsize) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2275; }
    { const char * tb_v2276 = "    tb_dict *d = tb_mem(sizeof(tb_dict));\n"; *(const char * *)tb_grow(v_lines) = tb_v2276; }
    { const char * tb_v2277 = "    d->text = text;\n"; *(const char * *)tb_grow(v_lines) = tb_v2277; }
    { const char * tb_v2278 = "    d->keys = tb_list_new(text ? sizeof(const char *) : sizeof(long long));\n"; *(const char * *)tb_grow(v_lines) = tb_v2278; }
    { const char * tb_v2279 = "    d->vals = tb_list_new(vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v2279; }
    { const char * tb_v2280 = "    d->slots = 0; d->pos = 0; d->npos = 0; d->base = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2280; }
    { const char * tb_v2281 = "    if (text) tb_dreindex(d, 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v2281; }
    { const char * tb_v2282 = "    else { d->mode = 1; d->nslots = 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2282; }
    { const char * tb_v2283 = "    return d;\n"; *(const char * *)tb_grow(v_lines) = tb_v2283; }
    { const char * tb_v2284 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2284; }
    { const char * tb_v2285 = "// .clear(): список снова пустой; свой кусок памяти отдаётся (если он не временный).\n"; *(const char * *)tb_grow(v_lines) = tb_v2285; }
    { const char * tb_v2286 = "// Тексты и obj внутри не удаляются: на них могут смотреть другие переменные.\n"; *(const char * *)tb_grow(v_lines) = tb_v2286; }
    { const char * tb_v2287 = "static void tb_lclear(tb_list *l, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2287; }
    { const char * tb_v2288 = "    if (l->cap < 0) tb_crash(line, \".clear() нельзя, пока по списку идёт each ... by N\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2288; }
    { const char * tb_v2289 = "    int temp = tb_nchunks > 0 && tb_in_temp(l->data);\n"; *(const char * *)tb_grow(v_lines) = tb_v2289; }
    { const char * tb_v2290 = "    if (!temp) free(l->data);\n"; *(const char * *)tb_grow(v_lines) = tb_v2290; }
    { const char * tb_v2291 = "    long long cap = l->size == 0 ? 64 : 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v2291; }
    { const char * tb_v2292 = "    long long bytes = l->size == 0 ? 8 : l->size * 4;\n"; *(const char * *)tb_grow(v_lines) = tb_v2292; }
    { const char * tb_v2293 = "    l->data = tb_in_cur_temp(l->data) ? tb_temp_alloc(bytes) : malloc(bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v2293; }
    { const char * tb_v2294 = "    if (l->data == 0) tb_crash(line, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2294; }
    { const char * tb_v2295 = "    memset(l->data, 0, bytes);\n"; *(const char * *)tb_grow(v_lines) = tb_v2295; }
    { const char * tb_v2296 = "    l->len = 0; l->cap = cap;\n"; *(const char * *)tb_grow(v_lines) = tb_v2296; }
    { const char * tb_v2297 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2297; }
    { const char * tb_v2298 = "static void tb_dclear(tb_dict *d, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2298; }
    { const char * tb_v2299 = "    tb_lclear(d->keys, line);\n"; *(const char * *)tb_grow(v_lines) = tb_v2299; }
    { const char * tb_v2300 = "    tb_lclear(d->vals, line);\n"; *(const char * *)tb_grow(v_lines) = tb_v2300; }
    { const char * tb_v2301 = "    if (d->pos && !(tb_nchunks > 0 && tb_in_temp(d->pos))) free(d->pos);\n"; *(const char * *)tb_grow(v_lines) = tb_v2301; }
    { const char * tb_v2302 = "    if (d->slots && !(tb_nchunks > 0 && tb_in_temp(d->slots))) free(d->slots);\n"; *(const char * *)tb_grow(v_lines) = tb_v2302; }
    { const char * tb_v2303 = "    d->slots = 0; d->pos = 0; d->npos = 0; d->base = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2303; }
    { const char * tb_v2304 = "    if (d->text) tb_dreindex(d, 8);\n"; *(const char * *)tb_grow(v_lines) = tb_v2304; }
    { const char * tb_v2305 = "    else { d->mode = 1; d->nslots = 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2305; }
    { const char * tb_v2306 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2306; }
    { const char * tb_v2307 = "// новый ключ int k: можно ли остаться без поиска (mode 1 или 2)\n"; *(const char * *)tb_grow(v_lines) = tb_v2307; }
    { const char * tb_v2308 = "static void tb_dplace_int(tb_dict *d, long long k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2308; }
    { const char * tb_v2309 = "    long long n = d->keys->len;          // уже вместе с новым ключом\n"; *(const char * *)tb_grow(v_lines) = tb_v2309; }
    { const char * tb_v2310 = "    if (d->mode == 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2310; }
    { const char * tb_v2311 = "        if (n == 1) { d->base = k; return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2311; }
    { const char * tb_v2312 = "        if (k == d->base + n - 1) return;  // подряд — остаёмся в mode 1\n"; *(const char * *)tb_grow(v_lines) = tb_v2312; }
    { const char * tb_v2313 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2313; }
    { const char * tb_v2314 = "    if (d->mode == 1 || d->mode == 2) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2314; }
    { const char * tb_v2315 = "        // плотно ли: ключи не дальше 4 ячеек на один ключ\n"; *(const char * *)tb_grow(v_lines) = tb_v2315; }
    { const char * tb_v2316 = "        long long o = k - d->base;\n"; *(const char * *)tb_grow(v_lines) = tb_v2316; }
    { const char * tb_v2317 = "        if (o >= 0 && o < 4 * n + 64) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2317; }
    { const char * tb_v2318 = "            if (d->mode == 2 && o < d->npos) { d->pos[o] = n; return; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2318; }
    { const char * tb_v2319 = "            long long want = d->npos * 2 > o + 1 ? d->npos * 2 : o + 1 + n;\n"; *(const char * *)tb_grow(v_lines) = tb_v2319; }
    { const char * tb_v2320 = "            if (want < 16) want = 16;\n"; *(const char * *)tb_grow(v_lines) = tb_v2320; }
    { const char * tb_v2321 = "            tb_dto_pos(d, want);\n"; *(const char * *)tb_grow(v_lines) = tb_v2321; }
    { const char * tb_v2322 = "            return;\n"; *(const char * *)tb_grow(v_lines) = tb_v2322; }
    { const char * tb_v2323 = "        }\n"; *(const char * *)tb_grow(v_lines) = tb_v2323; }
    { const char * tb_v2324 = "        tb_dto_hash(d);\n"; *(const char * *)tb_grow(v_lines) = tb_v2324; }
    { const char * tb_v2325 = "        return;\n"; *(const char * *)tb_grow(v_lines) = tb_v2325; }
    { const char * tb_v2326 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2326; }
    { const char * tb_v2327 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2327; }
    { const char * tb_v2328 = "// место под значение ключа k; если ключа нет — добавить со значением *def\n"; *(const char * *)tb_grow(v_lines) = tb_v2328; }
    { const char * tb_v2329 = "static void *tb_dput(tb_dict *d, const void *k, const void *def) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2329; }
    { const char * tb_v2330 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v2330; }
    { const char * tb_v2331 = "    if (at >= 0) return d->vals->data + at * d->vals->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v2331; }
    { const char * tb_v2332 = "    memcpy(tb_grow(d->keys), k, d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2332; }
    { const char * tb_v2333 = "    void *v = tb_grow(d->vals);\n"; *(const char * *)tb_grow(v_lines) = tb_v2333; }
    { const char * tb_v2334 = "    if (def) memcpy(v, def, d->vals->size); else memset(v, 0, d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2334; }
    { const char * tb_v2335 = "    if (d->mode != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2335; }
    { const char * tb_v2336 = "        // без поиска; если ключ лёг далеко, tb_dplace_int сам перейдёт на хеш и впишет все ключи\n"; *(const char * *)tb_grow(v_lines) = tb_v2336; }
    { const char * tb_v2337 = "        tb_dplace_int(d, *(const long long *)k);\n"; *(const char * *)tb_grow(v_lines) = tb_v2337; }
    { const char * tb_v2338 = "        return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v2338; }
    { const char * tb_v2339 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2339; }
    { const char * tb_v2340 = "    if (d->keys->len * 2 > d->nslots) tb_dreindex(d, d->nslots * 2);\n"; *(const char * *)tb_grow(v_lines) = tb_v2340; }
    { const char * tb_v2341 = "    else {\n"; *(const char * *)tb_grow(v_lines) = tb_v2341; }
    { const char * tb_v2342 = "        unsigned long long mask = d->nslots - 1, i = tb_hash(d, k) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v2342; }
    { const char * tb_v2343 = "        while (d->slots[i] != 0) i = (i + 1) & mask;\n"; *(const char * *)tb_grow(v_lines) = tb_v2343; }
    { const char * tb_v2344 = "        d->slots[i] = d->keys->len;\n"; *(const char * *)tb_grow(v_lines) = tb_v2344; }
    { const char * tb_v2345 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2345; }
    { const char * tb_v2346 = "    return v;\n"; *(const char * *)tb_grow(v_lines) = tb_v2346; }
    { const char * tb_v2347 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2347; }
    { const char * tb_v2348 = "static void *tb_dget(tb_dict *d, const void *k, int line) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2348; }
    { const char * tb_v2349 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v2349; }
    { const char * tb_v2350 = "    if (TB_RARE(at < 0)) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2350; }
    { const char * tb_v2351 = "        if (d->text) tb_crash(line, tb_cat(\"в dict нет ключа: \", *(const char * const *)k));\n"; *(const char * *)tb_grow(v_lines) = tb_v2351; }
    { const char * tb_v2352 = "        tb_crash(line, tb_cat(\"в dict нет ключа: \", tb_itos(*(const long long *)k)));\n"; *(const char * *)tb_grow(v_lines) = tb_v2352; }
    { const char * tb_v2353 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2353; }
    { const char * tb_v2354 = "    return d->vals->data + at * d->vals->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v2354; }
    { const char * tb_v2355 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2355; }
    { const char * tb_v2356 = "// together: шаг 1 — просим память принести место ключа заранее\n"; *(const char * *)tb_grow(v_lines) = tb_v2356; }
    { const char * tb_v2357 = "static inline unsigned long long tb_dpre1(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2357; }
    { const char * tb_v2358 = "    if (d->mode == 1) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2358; }
    { const char * tb_v2359 = "        unsigned long long at = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v2359; }
    { const char * tb_v2360 = "        if (at < (unsigned long long)d->vals->len) __builtin_prefetch(d->vals->data + at * d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2360; }
    { const char * tb_v2361 = "        return ~0ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v2361; }
    { const char * tb_v2362 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2362; }
    { const char * tb_v2363 = "    if (d->mode == 2) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2363; }
    { const char * tb_v2364 = "        unsigned long long o = (unsigned long long)(*(const long long *)k - d->base);\n"; *(const char * *)tb_grow(v_lines) = tb_v2364; }
    { const char * tb_v2365 = "        if (o < (unsigned long long)d->npos) { __builtin_prefetch(&d->pos[o]); return o; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2365; }
    { const char * tb_v2366 = "        return ~0ULL;\n"; *(const char * *)tb_grow(v_lines) = tb_v2366; }
    { const char * tb_v2367 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2367; }
    { const char * tb_v2368 = "    unsigned long long i = tb_hash(d, k) & (d->nslots - 1);\n"; *(const char * *)tb_grow(v_lines) = tb_v2368; }
    { const char * tb_v2369 = "    __builtin_prefetch(&d->slots[i]);\n"; *(const char * *)tb_grow(v_lines) = tb_v2369; }
    { const char * tb_v2370 = "    return i;\n"; *(const char * *)tb_grow(v_lines) = tb_v2370; }
    { const char * tb_v2371 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2371; }
    { const char * tb_v2372 = "// шаг 2 — место приехало: просим принести ключ и значение\n"; *(const char * *)tb_grow(v_lines) = tb_v2372; }
    { const char * tb_v2373 = "static inline void tb_dpre2(tb_dict *d, unsigned long long i) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2373; }
    { const char * tb_v2374 = "    if (i == ~0ULL || d->mode == 1) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v2374; }
    { const char * tb_v2375 = "    long long s = d->mode == 2 ? d->pos[i] : d->slots[i];\n"; *(const char * *)tb_grow(v_lines) = tb_v2375; }
    { const char * tb_v2376 = "    if (s != 0) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2376; }
    { const char * tb_v2377 = "        if (d->mode == 0) __builtin_prefetch(d->keys->data + (s - 1) * d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2377; }
    { const char * tb_v2378 = "        __builtin_prefetch(d->vals->data + (s - 1) * d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2378; }
    { const char * tb_v2379 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2379; }
    { const char * tb_v2380 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2380; }
    { const char * tb_v2381 = "// элемент списка заранее (prefetch за пределами списка ничего не ломает)\n"; *(const char * *)tb_grow(v_lines) = tb_v2381; }
    { const char * tb_v2382 = "static inline void tb_lpre(tb_list *l, long long i) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2382; }
    { const char * tb_v2383 = "    __builtin_prefetch(l->data + i * l->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2383; }
    { const char * tb_v2384 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2384; }
    { const char * tb_v2385 = "// ---------- холодные качества obj: своя память, подряд, отдельно от горячих ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2385; }
    { const char * tb_v2386 = "static void *tb_cold_mem(long long n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2386; }
    { const char * tb_v2387 = "    if (tb_temp_depth > 0) return tb_mem(n);\n"; *(const char * *)tb_grow(v_lines) = tb_v2387; }
    { const char * tb_v2388 = "    static _Thread_local char *p = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2388; }
    { const char * tb_v2389 = "    static _Thread_local long long left = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2389; }
    { const char * tb_v2390 = "    n = (n + 15) & ~15LL;\n"; *(const char * *)tb_grow(v_lines) = tb_v2390; }
    { const char * tb_v2391 = "    if (left < n) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2391; }
    { const char * tb_v2392 = "        long long size = n > 65536 ? n : 65536;\n"; *(const char * *)tb_grow(v_lines) = tb_v2392; }
    { const char * tb_v2393 = "        p = malloc(size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2393; }
    { const char * tb_v2394 = "        if (!p) tb_crash(0, \"не хватило памяти\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2394; }
    { const char * tb_v2395 = "        left = size;\n"; *(const char * *)tb_grow(v_lines) = tb_v2395; }
    { const char * tb_v2396 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2396; }
    { const char * tb_v2397 = "    void *r = p;\n"; *(const char * *)tb_grow(v_lines) = tb_v2397; }
    { const char * tb_v2398 = "    p += n;\n"; *(const char * *)tb_grow(v_lines) = tb_v2398; }
    { const char * tb_v2399 = "    left -= n;\n"; *(const char * *)tb_grow(v_lines) = tb_v2399; }
    { const char * tb_v2400 = "    memset(r, 0, n);\n"; *(const char * *)tb_grow(v_lines) = tb_v2400; }
    { const char * tb_v2401 = "    return r;\n"; *(const char * *)tb_grow(v_lines) = tb_v2401; }
    { const char * tb_v2402 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2402; }
    { const char * tb_v2403 = "// ---------- background: работа в своём потоке ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2403; }
    { const char * tb_v2404 = "typedef struct { pthread_t th; int done; void *res; } tb_task;\n"; *(const char * *)tb_grow(v_lines) = tb_v2404; }
    { const char * tb_v2405 = "static void tb_task_start(tb_task *t, void *(*run)(void *), void *arg) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2405; }
    { const char * tb_v2406 = "    t->done = 0;\n"; *(const char * *)tb_grow(v_lines) = tb_v2406; }
    { const char * tb_v2407 = "    if (pthread_create(&t->th, 0, run, arg) != 0) tb_crash(0, \"не получилось запустить background\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2407; }
    { const char * tb_v2408 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2408; }
    { const char * tb_v2409 = "// a.wait: дождаться потока (один раз) и отдать адрес ответа\n"; *(const char * *)tb_grow(v_lines) = tb_v2409; }
    { const char * tb_v2410 = "static void *tb_wait(tb_task *t) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2410; }
    { const char * tb_v2411 = "    if (!t->done) { pthread_join(t->th, 0); t->done = 1; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2411; }
    { const char * tb_v2412 = "    return t->res;\n"; *(const char * *)tb_grow(v_lines) = tb_v2412; }
    { const char * tb_v2413 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2413; }
    { const char * tb_v2414 = "static int tb_dhas(tb_dict *d, const void *k) { return tb_dfind(d, k) >= 0; }\n"; *(const char * *)tb_grow(v_lines) = tb_v2414; }
    { const char * tb_v2415 = "static void tb_dremove(tb_dict *d, const void *k) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2415; }
    { const char * tb_v2416 = "    long long at = tb_dfind(d, k);\n"; *(const char * *)tb_grow(v_lines) = tb_v2416; }
    { const char * tb_v2417 = "    if (at < 0) return;\n"; *(const char * *)tb_grow(v_lines) = tb_v2417; }
    { const char * tb_v2418 = "    if (d->mode != 0) tb_dto_hash(d);\n"; *(const char * *)tb_grow(v_lines) = tb_v2418; }
    { const char * tb_v2419 = "    long long last = d->keys->len - 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2419; }
    { const char * tb_v2420 = "    // на место удалённого ставим последний: порядок меняется только у него\n"; *(const char * *)tb_grow(v_lines) = tb_v2420; }
    { const char * tb_v2421 = "    memcpy(d->keys->data + at * d->keys->size, d->keys->data + last * d->keys->size, d->keys->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2421; }
    { const char * tb_v2422 = "    memcpy(d->vals->data + at * d->vals->size, d->vals->data + last * d->vals->size, d->vals->size);\n"; *(const char * *)tb_grow(v_lines) = tb_v2422; }
    { const char * tb_v2423 = "    d->keys->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2423; }
    { const char * tb_v2424 = "    d->vals->len -= 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2424; }
    { const char * tb_v2425 = "    tb_dreindex(d, d->nslots);\n"; *(const char * *)tb_grow(v_lines) = tb_v2425; }
    { const char * tb_v2426 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2426; }
    { const char * tb_v2427 = "static tb_dict *tb_dict_from(int text, long long vsize, long long n, const void *keys, const void *vals) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2427; }
    { const char * tb_v2428 = "    tb_dict *d = tb_dict_new(text, vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v2428; }
    { const char * tb_v2429 = "    long long ksize = d->keys->size;\n"; *(const char * *)tb_grow(v_lines) = tb_v2429; }
    { const char * tb_v2430 = "    for (long long i = 0; i < n; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2430; }
    { const char * tb_v2431 = "        memcpy(tb_dput(d, (const char *)keys + i * ksize, 0), (const char *)vals + i * vsize, vsize);\n"; *(const char * *)tb_grow(v_lines) = tb_v2431; }
    { const char * tb_v2432 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2432; }
    { const char * tb_v2433 = "    return d;\n"; *(const char * *)tb_grow(v_lines) = tb_v2433; }
    { const char * tb_v2434 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2434; }
    { const char * tb_v2435 = "\n"; *(const char * *)tb_grow(v_lines) = tb_v2435; }
    { const char * tb_v2436 = "// ---------- on all cores: цикл на всех ядрах ----------\n"; *(const char * *)tb_grow(v_lines) = tb_v2436; }
    { const char * tb_v2437 = "// Работа делится на куски; каждое ядро берёт следующий свободный кусок,\n"; *(const char * *)tb_grow(v_lines) = tb_v2437; }
    { const char * tb_v2438 = "// пока они не кончатся (так ядра не простаивают, если куски разные по тяжести).\n"; *(const char * *)tb_grow(v_lines) = tb_v2438; }
    { const char * tb_v2439 = "// shared: каждое ядро копит своё на «листке», потом один раз добавляет под замком.\n"; *(const char * *)tb_grow(v_lines) = tb_v2439; }
    { const char * tb_v2440 = "static pthread_mutex_t tb_par_lock = PTHREAD_MUTEX_INITIALIZER;\n"; *(const char * *)tb_grow(v_lines) = tb_v2440; }
    { const char * tb_v2441 = "static int tb_cores(void) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2441; }
    { const char * tb_v2442 = "    const char *want = getenv(\"TB_CORES\");   // TB_CORES=1 — проверить на одном ядре\n"; *(const char * *)tb_grow(v_lines) = tb_v2442; }
    { const char * tb_v2443 = "    long n = want != 0 ? atol(want) : sysconf(_SC_NPROCESSORS_ONLN);\n"; *(const char * *)tb_grow(v_lines) = tb_v2443; }
    { const char * tb_v2444 = "    return n < 1 ? 1 : (n > 256 ? 256 : (int)n);\n"; *(const char * *)tb_grow(v_lines) = tb_v2444; }
    { const char * tb_v2445 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2445; }
    { const char * tb_v2446 = "static void tb_par_run(void *(*work)(void *), void *job, long long total) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2446; }
    { const char * tb_v2447 = "    int n = tb_cores();\n"; *(const char * *)tb_grow(v_lines) = tb_v2447; }
    { const char * tb_v2448 = "    if (n > total) n = (int)total;\n"; *(const char * *)tb_grow(v_lines) = tb_v2448; }
    { const char * tb_v2449 = "    if (n < 1) n = 1;\n"; *(const char * *)tb_grow(v_lines) = tb_v2449; }
    { const char * tb_v2450 = "    pthread_t t[256];\n"; *(const char * *)tb_grow(v_lines) = tb_v2450; }
    { const char * tb_v2451 = "    for (int i = 1; i < n; i++) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2451; }
    { const char * tb_v2452 = "        if (pthread_create(&t[i], 0, work, job) != 0) tb_crash(0, \"не получилось запустить ядро\");\n"; *(const char * *)tb_grow(v_lines) = tb_v2452; }
    { const char * tb_v2453 = "    }\n"; *(const char * *)tb_grow(v_lines) = tb_v2453; }
    { const char * tb_v2454 = "    work(job);\n"; *(const char * *)tb_grow(v_lines) = tb_v2454; }
    { const char * tb_v2455 = "    for (int i = 1; i < n; i++) pthread_join(t[i], 0);\n"; *(const char * *)tb_grow(v_lines) = tb_v2455; }
    { const char * tb_v2456 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2456; }
    { const char * tb_v2457 = "static long long tb_par_step(long long total) {\n"; *(const char * *)tb_grow(v_lines) = tb_v2457; }
    { const char * tb_v2458 = "    long long s = total / ((long long)tb_cores() * 16);\n"; *(const char * *)tb_grow(v_lines) = tb_v2458; }
    { const char * tb_v2459 = "    return s < 1 ? 1 : s;\n"; *(const char * *)tb_grow(v_lines) = tb_v2459; }
    { const char * tb_v2460 = "}\n"; *(const char * *)tb_grow(v_lines) = tb_v2460; }
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
