// Tachbit под FuturaOS (ОС Алексея на базе ToaruOS).
// Этот файл подключается раньше всего остального (clang -include).
// Заголовки ToaruOS не знают, как устроены замки в libc FuturaOS,
// поэтому Tachbit не трогает его pthread_mutex/pthread_cond, а использует
// свои маленькие замки на атомиках + sched_yield. Потоки создаёт его pthread_create.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sched.h>
#include <pthread.h>
#include <sys/resource.h>

// --- то, что есть в libc.a FuturaOS, но нет в заголовках ToaruOS ---
long long atoll(const char *s);
int posix_memalign(void **p, size_t align, size_t n);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t n);

#ifndef RLIMIT_STACK
struct rlimit { unsigned long rlim_cur, rlim_max; };
#define RLIMIT_STACK 3
#define RLIM_INFINITY (~0UL)
static inline int tb_fut_getrlimit(int r, struct rlimit *l) { (void)r; (void)l; return -1; }
#define getrlimit tb_fut_getrlimit
#endif

// --- свой замок ---
typedef struct { volatile int v; } tb_fut_mutex;
#define TB_FUT_MUTEX_INIT { 0 }
static inline int tb_fut_lock(tb_fut_mutex *m) {
    int n = 0;
    while (__atomic_exchange_n(&m->v, 1, __ATOMIC_ACQUIRE)) {
        if (++n < 64) __builtin_ia32_pause(); else sched_yield();
    }
    return 0;
}
static inline int tb_fut_unlock(tb_fut_mutex *m) { __atomic_store_n(&m->v, 0, __ATOMIC_RELEASE); return 0; }
static inline int tb_fut_mutex_init(tb_fut_mutex *m, void *a) { (void)a; m->v = 0; return 0; }

// --- своё «ждать сигнала» ---
typedef struct { volatile unsigned seq; } tb_fut_cond;
#define TB_FUT_COND_INIT { 0 }
static inline int tb_fut_cond_init(tb_fut_cond *c, void *a) { (void)a; c->seq = 0; return 0; }
static inline int tb_fut_cond_wait(tb_fut_cond *c, tb_fut_mutex *m) {
    unsigned s = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    tb_fut_unlock(m);
    int n = 0;
    while (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) == s) {
        if (++n < 64) __builtin_ia32_pause(); else sched_yield();
    }
    return tb_fut_lock(m);
}
static inline int tb_fut_cond_signal(tb_fut_cond *c) { __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE); return 0; }

#define pthread_mutex_t tb_fut_mutex
#define pthread_cond_t tb_fut_cond
#undef PTHREAD_MUTEX_INITIALIZER
#undef PTHREAD_COND_INITIALIZER
#define PTHREAD_MUTEX_INITIALIZER TB_FUT_MUTEX_INIT
#define PTHREAD_COND_INITIALIZER TB_FUT_COND_INIT
#define pthread_mutex_init tb_fut_mutex_init
#define pthread_mutex_lock tb_fut_lock
#define pthread_mutex_unlock tb_fut_unlock
#define pthread_cond_init tb_fut_cond_init
#define pthread_cond_wait tb_fut_cond_wait
#define pthread_cond_signal tb_fut_cond_signal
#define pthread_cond_broadcast tb_fut_cond_signal

// --- сеть: заголовки ToaruOS знают только IPv4 (и AF_INET там = 1) ---
// AF_INET6 = -1: socket() вернёт ошибку, и Tachbit сам возьмёт IPv4.
#include <sys/socket.h>
#include <netinet/in.h>
#ifndef AF_INET6
#define AF_INET6 (-1)
#define IPPROTO_IPV6 41
#define IPV6_V6ONLY 26
struct in6_addr { unsigned char s6_addr[16]; };
struct sockaddr_in6 { short sin6_family; unsigned short sin6_port; unsigned sin6_flowinfo; struct in6_addr sin6_addr; unsigned sin6_scope_id; };
static const struct in6_addr in6addr_any = { { 0 } };
#endif
#ifndef INADDR_ANY
#define INADDR_ANY 0u
#endif
#ifndef htons
#define htons(x) __builtin_bswap16((unsigned short)(x))
#define ntohs(x) __builtin_bswap16((unsigned short)(x))
#define htonl(x) __builtin_bswap32((unsigned)(x))
#define ntohl(x) __builtin_bswap32((unsigned)(x))
#endif

// --- syscall(): своего syscall() в libc FuturaOS нет; номера вызовов у него свои ---
#include <errno.h>
// errno: в ToaruOS это __errno_addr(), в libc FuturaOS — __errno_location()
int *__errno_location(void);
#undef errno
#define errno (*__errno_location())
static inline long tb_fut_syscall(long n, ...) { (void)n; errno = 38; return -1; }  // 38 = ENOSYS
#define syscall tb_fut_syscall

// --- mmap: ядро FuturaOS ставит на страницы без PROT_EXEC бит NX, но (похоже) не включает
// NX в процессоре (EFER.NXE) — тогда запись в такую страницу падает с «reserved bit».
// Обход: Tachbit просит у mmap страницы сразу с PROT_EXEC.
#include <sys/mman.h>
static inline void *tb_fut_mmap(void *a, size_t n, int prot, int flags, int fd, long off) {
    return mmap(a, n, prot | PROT_EXEC, flags, fd, off);
}
#define mmap tb_fut_mmap

// --- printf: libc FuturaOS печатает 0.0 через %g как "0e-1074" (ошибка в его libc).
// Чиним только этот случай: всё остальное печатается как есть.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
static inline int tb_fut_snprintf(char *s, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    if (s && n) {
        char *p;
        while ((p = strstr(s, "0e-1074")) != 0) { memmove(p + 1, p + 7, strlen(p + 7) + 1); r -= 6; }
    }
    return r;
}
#define snprintf tb_fut_snprintf
static inline int tb_fut_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (r < 0) return r;
    if ((size_t)r >= sizeof buf) {   // длинная строка: печатаем как есть
        va_start(ap, fmt); r = vprintf(fmt, ap); va_end(ap); return r;
    }
    char *p;
    while ((p = strstr(buf, "0e-1074")) != 0) { memmove(p + 1, p + 7, strlen(p + 7) + 1); r -= 6; }
    fputs(buf, stdout);
    return r;
}
#define printf tb_fut_printf
