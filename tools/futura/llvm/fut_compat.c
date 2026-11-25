/* FuturaOS: страницы без PROT_EXEC его ядро помечает NX, но NX в процессоре не включён —
   любое обращение к такой странице падает (reserved bit). Поэтому всегда просим PROT_EXEC. */
#include <sys/mman.h>
#include <stddef.h>
void *__real_mmap(void *a, size_t n, int prot, int flags, int fd, long off);
int __real_mprotect(void *a, size_t n, int prot);
void *__wrap_mmap(void *a, size_t n, int prot, int flags, int fd, long off) {
    if (prot != PROT_NONE) prot |= PROT_EXEC;
    return __real_mmap(a, n, prot, flags, fd, off);
}
int __wrap_mprotect(void *a, size_t n, int prot) {
    if (prot != PROT_NONE) prot |= PROT_EXEC;
    return __real_mprotect(a, n, prot);
}
