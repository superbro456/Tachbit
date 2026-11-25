// Tachbit под FuturaOS, сборка с НАСТОЯЩИМИ заголовками Алексея (его include/).
// Подключается раньше всего (clang -include). Здесь только то, чего в его заголовках нет
// или что в его ОС работает не так. Замки — его собственные pthread_mutex/pthread_cond.
#pragma once
#ifdef __TINYC__
// tcc внутри FuturaOS. Он считает себя «Linux» — это не так, иначе Tachbit возьмёт mremap и т.п.
#undef __linux__
#undef __linux
#undef linux
#undef __gnu_linux__
#include <stdatomic.h>   // у tcc __ATOMIC_* и __atomic_load_n/store_n/... заданы здесь
// чего у tcc нет из встроенных слов gcc/clang
#define __atomic_exchange_n(p, v, o) ({ __typeof__(*(p)) __tv = (v), __tr; __atomic_exchange((p), &__tv, &__tr, (o)); __tr; })
#define __sync_lock_test_and_set(p, v) __atomic_exchange_n((p), (v), __ATOMIC_ACQUIRE)
#define __sync_lock_release(p) __atomic_store_n((p), 0, __ATOMIC_RELEASE)
#define __builtin_ia32_pause() __asm__ __volatile__("pause")
#define __builtin_prefetch(...) ((void)0)
#define __builtin_unwind_init() ((void)0)   // регистры и так сохраняет setjmp рядом
static inline unsigned short tb_fut_bswap16(unsigned short x) { return (unsigned short)((x >> 8) | (x << 8)); }
static inline unsigned tb_fut_bswap32(unsigned x) { return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24); }
static inline unsigned long long tb_fut_bswap64(unsigned long long x) { return ((unsigned long long)tb_fut_bswap32((unsigned)x) << 32) | tb_fut_bswap32((unsigned)(x >> 32)); }
#define __builtin_bswap16 tb_fut_bswap16
#define __builtin_bswap32 tb_fut_bswap32
#define __builtin_bswap64 tb_fut_bswap64
// переполнение: Tachbit зовёт их только для long long
static inline int tb_fut_addo(long long a, long long b, long long *r) { unsigned long long u = (unsigned long long)a + (unsigned long long)b; *r = (long long)u; return ((a ^ *r) & (b ^ *r)) < 0; }
static inline int tb_fut_subo(long long a, long long b, long long *r) { unsigned long long u = (unsigned long long)a - (unsigned long long)b; *r = (long long)u; return ((a ^ b) & (a ^ *r)) < 0; }
static inline int tb_fut_mulo(long long a, long long b, long long *r) {
    *r = (long long)((unsigned long long)a * (unsigned long long)b);
    if (a == 0 || b == 0) return 0;
    if ((a == -1 && b == (-9223372036854775807LL - 1)) || (b == -1 && a == (-9223372036854775807LL - 1))) return 1;
    return *r / b != a;
}
#define __builtin_add_overflow tb_fut_addo
#define __builtin_sub_overflow tb_fut_subo
#define __builtin_mul_overflow tb_fut_mulo
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// --- сеть: IPv6-адреса есть, но нет in6addr_any и INADDR_ANY ---
#ifndef INADDR_ANY
#define INADDR_ANY 0u
#endif
static const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
// IPv6 в заголовках есть, но его сокеты на адресе IPv6 отвечают «Bad address».
// AF_INET6 = -1: socket() вернёт ошибку, и Tachbit сам возьмёт IPv4.
#undef AF_INET6
#define AF_INET6 (-1)

// --- popen/pclose: в его libc.so их нет (есть только в libc.a) — свои, через pipe + fork ---
#include <unistd.h>
#include <sys/wait.h>
static int tb_fut_popen_pid[64];
static inline FILE *tb_fut_popen(const char *cmd, const char *mode) {
    int fd[2];
    if (pipe(fd) != 0) return 0;
    int rd = mode[0] == 'r';
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return 0; }
    if (pid == 0) {
        if (rd) dup2(fd[1], 1); else dup2(fd[0], 0);
        close(fd[0]); close(fd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(127);
    }
    int mine = rd ? fd[0] : fd[1];
    close(rd ? fd[1] : fd[0]);
    if (mine >= 0 && mine < 64) tb_fut_popen_pid[mine] = pid;
    return fdopen(mine, rd ? "r" : "w");
}
static inline int tb_fut_pclose(FILE *f) {
    int fd = fileno(f), st = 0;
    fclose(f);
    if (fd < 0 || fd >= 64) return -1;
    if (waitpid(tb_fut_popen_pid[fd], &st, 0) < 0) return -1;
    return st;
}
#define popen tb_fut_popen
#define pclose tb_fut_pclose

// --- getrlimit в его libc нет ---
struct rlimit { unsigned long rlim_cur, rlim_max; };
#define RLIMIT_STACK 3
#define RLIM_INFINITY (~0UL)
static inline int tb_fut_getrlimit(int r, struct rlimit *l) { (void)r; (void)l; errno = 38; return -1; }
#define getrlimit tb_fut_getrlimit

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

// --- syscall(): в его libc функции syscall() нет, но вызов ядра простой:
// инструкция syscall, номер в rax, аргументы rdi rsi rdx r10 r8 r9, ответ <0 = -ошибка.
// Номера — его собственные (syscall_nums.h), не Linux.
static inline long tb_fut_syscall(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long r = n;
    __asm__ __volatile__("syscall" : "+a"(r) : "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
    return r;
}
#define syscall(n, ...) tb_fut_syscall_v(n, ##__VA_ARGS__, 0, 0, 0, 0, 0, 0)
#define tb_fut_syscall_v(n, a, b, c, d, e, f, ...) tb_fut_syscall((long)(n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

// --- mmap: страницы без PROT_EXEC в его ядре падают при записи («reserved bit»):
// похоже, ядро ставит бит NX, но не включает EFER.NXE. Обход: всегда PROT_EXEC.
static inline void *tb_fut_mmap(void *a, size_t n, int prot, int flags, int fd, long off) {
    return mmap(a, n, prot | PROT_EXEC, flags, fd, off);
}
#define mmap tb_fut_mmap

// --- printf("%g", 0.0) в его libc печатает "0e-1074". Чиним только этот случай.
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
    if ((size_t)r >= sizeof buf) { va_start(ap, fmt); r = vprintf(fmt, ap); va_end(ap); return r; }
    char *p;
    while ((p = strstr(buf, "0e-1074")) != 0) { memmove(p + 1, p + 7, strlen(p + 7) + 1); r -= 6; }
    fputs(buf, stdout);
    return r;
}
#define printf tb_fut_printf
