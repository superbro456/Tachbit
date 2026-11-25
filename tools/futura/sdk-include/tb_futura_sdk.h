// Tachbit под FuturaOS, сборка с НАСТОЯЩИМИ заголовками Алексея (его include/).
// Подключается раньше всего (clang -include). Здесь только то, чего в его заголовках нет
// или что в его ОС работает не так. Замки — его собственные pthread_mutex/pthread_cond.
#pragma once
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

// --- есть в его libc, но нет в заголовках ---
FILE *popen(const char *cmd, const char *mode);
int pclose(FILE *f);

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
