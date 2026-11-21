// Библиотека ядра Омниума (tb bare): то, что у обычной программы даёт ОС и libc.
// Загрузка (multiboot → 64 бита), своя маленькая libc, память и квоты клеток, прерывания,
// задачи и планировщик, страницы памяти, несколько ядер процессора (ACPI, APIC).
// Компилятор вставляет этот файл в compiler/runtime.h на место строки //@include (tools/embed_runtime.py).

// ---------- режим без ОС (tb bare): своя маленькая библиотека вместо libc ----------
// Программа сама себе ОС: загружается через multiboot (QEMU, GRUB), пишет на экран
// (текстовый режим VGA) и в последовательный порт COM1, память берёт из своего запаса.
// Есть прерывания, таймер, клавиатура, задачи (background) и страницы памяти; файлов нет.
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
// «переменные потока» — настоящие: у каждой задачи свой блок, на него смотрит регистр FS
// (так их не надо копировать при смене задачи, и на нескольких ядрах процессора у каждого свои)
#define _Thread_local __thread __attribute__((tls_model("local-exec")))
#define EOF (-1)
typedef struct tb_file FILE;
#define stdout ((FILE *)1)
#define stderr ((FILE *)2)
typedef long pthread_t;   // 8 байт, как адрес: раскладка задачи та же, что в обычной программе
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define pthread_mutex_lock(m) ((void)(m))
#define pthread_mutex_unlock(m) ((void)(m))
#define _SC_NPROCESSORS_ONLN 0

TB_INL void tb_outb(unsigned short port, unsigned char v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
TB_INL unsigned char tb_inb(unsigned short port) { unsigned char v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
TB_INL void tb_outw(unsigned short port, unsigned short v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
TB_INL unsigned short tb_inw(unsigned short port) { unsigned short v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
TB_INL void tb_outl(unsigned short port, unsigned int v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
TB_INL unsigned int tb_inl(unsigned short port) { unsigned int v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
// ядро процессора («звезда»): его номер, кого он сейчас ведёт, свой стек для пустого хода.
// Регистр GS у каждого ядра смотрит на его запись (в самом начале — адрес её самой)
#define TB_NCPU 16
typedef struct tb_cpu_s {
    struct tb_cpu_s *self;
    int id, apic, run, slice, depth, prev;   // run: номер задачи (-1 — пустой ход); depth: вложенность «общего замка»; prev: задача, которую отпустить после смены стека
    uint64_t *idle_frame;
    unsigned char irq_tls[512 + 16] __attribute__((aligned(16)));   // переменные потока для функций прерывания
    unsigned char idle_stack[16384] __attribute__((aligned(16)));
    unsigned char df_stack[16384] __attribute__((aligned(16)));
} tb_cpu;
TB_API tb_cpu tb_cpus[TB_NCPU];
TB_API int tb_ncpu = 1;
TB_INL tb_cpu *tb_me(void) { tb_cpu *c; __asm__ volatile("movq %%gs:0, %0" : "=r"(c)); return c; }
// короткое дело библиотеки (память, экран, список задач): прерывания выключены и взят общий
// замок — так его не делят ни прерывание, ни другое ядро процессора. Можно вкладывать
TB_API volatile int tb_bkl = 0;
TB_INL uint64_t tb_irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq\npopq %0\ncli" : "=r"(f) : : "memory");
    tb_cpu *c = tb_me();
    if (c->depth++ == 0)
        while (__atomic_exchange_n(&tb_bkl, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
    return f;
}
TB_INL void tb_irq_restore(uint64_t f) {
    tb_cpu *c = tb_me();
    if (--c->depth == 0) __atomic_store_n(&tb_bkl, 0, __ATOMIC_RELEASE);
    __asm__ volatile("pushq %0\npopfq" : : "r"(f) : "memory", "cc");
}
// отдать общий замок целиком (задача уходит навсегда — падает или кончилась)
TB_INL void tb_bkl_drop(void) {
    tb_cpu *c = tb_me();
    if (c->depth > 0) { c->depth = 0; __atomic_store_n(&tb_bkl, 0, __ATOMIC_RELEASE); }
}

// экран 80×25: байт буквы и байт цвета; русские буквы рисуем своим шрифтом (см. tb_vga_font)
TB_API volatile unsigned short *const tb_vga = (volatile unsigned short *)0xB8000;
TB_API int tb_vrow = 0, tb_vcol = 0;
TB_API unsigned char tb_vcolor = 0x0A;   // светло-зелёный на чёрном
TB_API void tb_vga_put(unsigned char ch) {
    if (ch == '\n') { tb_vcol = 0; tb_vrow++; }
    else if (ch == '\b') { if (tb_vcol > 0) tb_vcol--; tb_vga[tb_vrow * 80 + tb_vcol] = (unsigned short)(tb_vcolor << 8 | ' '); }
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
// русские буквы: свои картинки 8×16 кладём в шрифт VGA на места 128..193
TB_API const unsigned char tb_cyr_font[66][16] = {
    {0x00, 0x00, 0x1c, 0x1c, 0x3c, 0x34, 0x36, 0x66, 0x7e, 0x63, 0x63, 0xc3, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x7f, 0x60, 0x60, 0x60, 0x7e, 0x67, 0x63, 0x63, 0x67, 0x7e, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x7e, 0x66, 0x67, 0x66, 0x7e, 0x66, 0x63, 0x63, 0x67, 0x7e, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x7f, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x7e, 0x76, 0x76, 0x76, 0x76, 0x76, 0x76, 0x66, 0x66, 0xff, 0xc3, 0xc3, 0x00, 0x00},
    {0x00, 0x00, 0x7f, 0x60, 0x60, 0x60, 0x7e, 0x60, 0x60, 0x60, 0x60, 0x7f, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0xdb, 0x7b, 0x7e, 0x3e, 0x3e, 0x7e, 0x7e, 0x7b, 0xdb, 0xdb, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x3c, 0x46, 0x06, 0x06, 0x1c, 0x06, 0x06, 0x06, 0x46, 0x3c, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x67, 0x67, 0x6f, 0x6f, 0x6b, 0x7b, 0x73, 0x73, 0x73, 0x63, 0x00, 0x00, 0x00, 0x00},
    {0x1c, 0x00, 0x67, 0x67, 0x6f, 0x6f, 0x6b, 0x7b, 0x73, 0x73, 0x73, 0x63, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x63, 0x66, 0x6c, 0x78, 0x78, 0x7c, 0x6e, 0x66, 0x67, 0x63, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x7f, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x67, 0x67, 0xc7, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x67, 0x77, 0x77, 0x7f, 0x5b, 0x5b, 0x43, 0x43, 0x43, 0x43, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x67, 0x67, 0x67, 0x67, 0x7f, 0x67, 0x67, 0x67, 0x67, 0x67, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x3c, 0x66, 0x67, 0x63, 0x63, 0x63, 0x63, 0x67, 0x66, 0x3c, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x7f, 0x67, 0x67, 0x67, 0x67, 0x67, 0x67, 0x67, 0x67, 0x67, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x7e, 0x67, 0x63, 0x63, 0x67, 0x7e, 0x60, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x1e, 0x30, 0x70, 0x60, 0x60, 0x60, 0x60, 0x70, 0x30, 0x1e, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x7f, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0xe3, 0x67, 0x36, 0x3e, 0x1c, 0x1c, 0x1c, 0x18, 0x78, 0x70, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x18, 0x3e, 0x7b, 0xdb, 0xdb, 0xdb, 0xdb, 0x7b, 0x3e, 0x18, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0xe3, 0x66, 0x36, 0x3c, 0x1c, 0x1c, 0x3c, 0x36, 0x66, 0xe3, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0xe6, 0xe6, 0xe6, 0xe6, 0xe6, 0xe6, 0xe6, 0xe6, 0xe6, 0xff, 0x03, 0x03, 0x00, 0x00},
    {0x00, 0x00, 0x63, 0x63, 0x63, 0x67, 0x7f, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xff, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xff, 0x01, 0x01, 0x00, 0x00},
    {0x00, 0x00, 0xf0, 0x30, 0x30, 0x30, 0x3e, 0x37, 0x33, 0x33, 0x3f, 0x3e, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0xc3, 0xc3, 0xc3, 0xc3, 0xfb, 0xdf, 0xcf, 0xcf, 0xfb, 0xf3, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x60, 0x60, 0x60, 0x60, 0x7c, 0x66, 0x67, 0x67, 0x66, 0x7c, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x7c, 0x7e, 0x06, 0x06, 0x7e, 0x3e, 0x06, 0x06, 0x7e, 0x7c, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0xce, 0xdb, 0xdb, 0xdb, 0xfb, 0xdb, 0xdb, 0xdb, 0xcb, 0xce, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x3f, 0x73, 0x63, 0x63, 0x73, 0x3f, 0x3b, 0x33, 0x73, 0x63, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x3e, 0x46, 0x07, 0x3f, 0x67, 0x67, 0x67, 0x3f, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x3e, 0x60, 0x7c, 0xe6, 0xe7, 0x63, 0x63, 0x67, 0x66, 0x3c, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x7e, 0x66, 0x66, 0x7e, 0x66, 0x63, 0x66, 0x7e, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x7e, 0x70, 0x70, 0x70, 0x70, 0x70, 0x70, 0x70, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x7e, 0x76, 0x76, 0x76, 0x76, 0x66, 0x66, 0xff, 0xc1, 0xc1, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x3c, 0x66, 0x63, 0x7f, 0x60, 0x60, 0x71, 0x3e, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0xdb, 0x7f, 0x7e, 0x3e, 0x7e, 0x7f, 0x5b, 0xdb, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x7c, 0x06, 0x06, 0x1c, 0x06, 0x06, 0x06, 0x7c, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x67, 0x67, 0x6f, 0x6f, 0x7f, 0x77, 0x77, 0x67, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x24, 0x1c, 0x00, 0x67, 0x67, 0x6f, 0x6f, 0x7f, 0x77, 0x77, 0x67, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x67, 0x6e, 0x7c, 0x7c, 0x7c, 0x66, 0x66, 0x63, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x3f, 0x33, 0x33, 0x33, 0x33, 0x33, 0x63, 0xe3, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x67, 0x67, 0x77, 0x7f, 0x5f, 0x5b, 0x5b, 0x43, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x7e, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x3c, 0x66, 0x67, 0x63, 0x63, 0x67, 0x66, 0x3c, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x7e, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x7e, 0x76, 0x63, 0x63, 0x63, 0x63, 0x76, 0x7e, 0x60, 0x60, 0x60, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x1e, 0x32, 0x60, 0x60, 0x60, 0x60, 0x32, 0x1e, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x7e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x63, 0x67, 0x66, 0x36, 0x3e, 0x3c, 0x1c, 0x1c, 0x18, 0x18, 0x70, 0x00},
    {0x00, 0x18, 0x18, 0x18, 0x3c, 0x7a, 0x5b, 0x5b, 0x5b, 0x5b, 0x7a, 0x3c, 0x18, 0x18, 0x18, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x67, 0x36, 0x3c, 0x1c, 0x1c, 0x3c, 0x76, 0x67, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7f, 0x03, 0x03, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3e, 0x06, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xff, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xdb, 0xff, 0x01, 0x01, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x30, 0x3e, 0x3f, 0x33, 0x3f, 0x3e, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0xc3, 0xc3, 0xc3, 0xf3, 0xfb, 0xdb, 0xfb, 0xf3, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x60, 0x60, 0x60, 0x7c, 0x66, 0x66, 0x66, 0x7c, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x38, 0x7c, 0x06, 0x3e, 0x3e, 0x06, 0x7e, 0x7c, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0xce, 0xda, 0xdb, 0xf3, 0xd3, 0xdb, 0xda, 0xce, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x3f, 0x73, 0x63, 0x73, 0x3f, 0x3b, 0x33, 0x63, 0x00, 0x00, 0x00, 0x00},
    {0x6c, 0x00, 0x7f, 0x60, 0x60, 0x60, 0x7e, 0x60, 0x60, 0x60, 0x60, 0x7f, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x6c, 0x00, 0x3c, 0x66, 0x63, 0x7f, 0x60, 0x60, 0x71, 0x3e, 0x00, 0x00, 0x00, 0x00},
};
TB_API void tb_vga_font(void) {
    tb_outw(0x3C4, 0x0402); tb_outw(0x3C4, 0x0704);   // пишем в слой шрифта
    tb_outw(0x3CE, 0x0204); tb_outw(0x3CE, 0x0005); tb_outw(0x3CE, 0x0406);
    volatile unsigned char *font = (volatile unsigned char *)0xA0000;
    for (int i = 0; i < 66; i++)
        for (int y = 0; y < 16; y++) font[(128 + i) * 32 + y] = tb_cyr_font[i][y];
    tb_outw(0x3C4, 0x0302); tb_outw(0x3C4, 0x0304);   // обратно в текстовый режим
    tb_outw(0x3CE, 0x0004); tb_outw(0x3CE, 0x1005); tb_outw(0x3CE, 0x0E06);
}
TB_API unsigned char tb_utf_lead = 0;
TB_API void tb_putc(char c) {
    unsigned char ch = (unsigned char)c;
    while ((tb_inb(0x3F8 + 5) & 0x20) == 0) {}
    tb_outb(0x3F8, ch);   // в порт — как есть (UTF-8)
    if (ch < 0x80) { tb_utf_lead = 0; tb_vga_put(ch); return; }
    if (ch >= 0xC0) { tb_utf_lead = ch; return; }
    // второй байт: А..я = U+0410..U+044F, Ё = U+0401, ё = U+0451
    unsigned cp = ((unsigned)(tb_utf_lead & 0x1F) << 6) | (ch & 0x3F);
    if (tb_utf_lead == 0) return;
    tb_utf_lead = 0;
    if (cp >= 0x410 && cp <= 0x44F) tb_vga_put((unsigned char)(128 + cp - 0x410));
    else if (cp == 0x401) tb_vga_put(128 + 64);
    else if (cp == 0x451) tb_vga_put(128 + 65);
    else tb_vga_put('?');
}
// строка не рвётся: пока задача не дописала свою строку до '\n', другие ждут (недолго —
// если задача так и не допишет, через ~20 мс пишут всё равно)
TB_API volatile long long tb_line_owner = 0;
TB_API long long tb_out_me(void);
TB_API int fputs(const char *s, FILE *f) {
    if (!*s) return 0;
    long long me = tb_out_me();
    uint64_t fl;
    for (long spins = 0;; spins++) {
        fl = tb_irq_save();
        long long o = tb_line_owner;
        if (o == 0 || o == me || me < 0 || spins > 4000000) break;
        tb_irq_restore(fl);
        __builtin_ia32_pause();
    }
    unsigned char was = tb_vcolor;
    if (f == stderr) tb_vcolor = 0x0C;   // ошибки — красным
    char last = 0;
    while (*s) { last = *s; tb_putc(*s++); }
    tb_vcolor = was;
    if (me >= 0) tb_line_owner = last == '\n' ? 0 : me;
    tb_irq_restore(fl);
    return 0;
}
TB_API int fflush(FILE *f) { (void)f; return 0; }
TB_API int isatty(int fd) { (void)fd; return 0; }
TB_API char *getenv(const char *n) { (void)n; return 0; }

// выход: QEMU с -device isa-debug-exit закрывается, без него процессор просто спит
__attribute__((noreturn)) static void exit(int code) {
    tb_outb(0xF4, (unsigned char)code);
    for (;;) __asm__ volatile("cli; hlt");
}

// память: запас 64 МБ. Куски по размерам 16, 32, 64, ... байт; отданный кусок
// кладётся в корзину своего размера и выдаётся снова. Последний кусок отдаётся сразу.
#define TB_HEAP (64 << 20)
TB_API unsigned char tb_heap[TB_HEAP] __attribute__((aligned(16), section(".tbheap")));
TB_API size_t tb_heap_top = 0;
TB_API void *tb_bins[32];
// заголовок куска: размер (номер 4..31) + метка «это начало куска» — так по адресу видно,
// наш ли это кусок (для передачи владения), и хозяин (клетка) рядом
#define TB_HMAGIC 0x0B1A0C0000000000ull
#define TB_TAG_FREE (~0ull)
// запас памяти (как зона Service в старой Омниум): последние 8 МБ кучи берут только ядро,
// прерывания и службы. Обычные клетки упираются раньше — и службы живут, даже когда клетки съели всё
#define TB_RESERVE (8 << 20)
TB_API size_t tb_heap_limit(void);
TB_API int tb_cls(size_t n) { int k = 4; while (k < 31 && ((size_t)1 << k) < n) k++; return k; }
TB_API void *tb_malloc0(size_t n) {
    int k = tb_cls(n);
    if (tb_bins[k]) { void *p = tb_bins[k]; tb_bins[k] = *(void **)p; return p; }
    size_t need = ((size_t)1 << k) + 16;
    if (tb_heap_top + need > tb_heap_limit()) return 0;
    unsigned char *h = tb_heap + tb_heap_top;
    *(size_t *)h = (size_t)k | TB_HMAGIC;
    tb_heap_top += need;
    return h + 16;
}
TB_API void tb_free0(void *p) {
    if (p == 0) return;
    unsigned char *h = (unsigned char *)p - 16;
    int k = (int)(*(size_t *)h & 0xFF);
    if (h + ((size_t)1 << k) + 16 == tb_heap + tb_heap_top) { tb_heap_top = (size_t)(h - tb_heap); return; }
    *(void **)p = tb_bins[k];
    tb_bins[k] = p;
}
// квота клетки: каждый кусок помнит хозяина (в заголовке, за размером); у клетки — счёт байт.
// Клетка, взявшая больше своей квоты, падает одна (main и прерывания без квоты)
#define TB_CELL_QUOTA (16 << 20)
TB_API uint64_t tb_cell_charge(size_t bytes);
TB_API void tb_cell_refund(uint64_t tag, long long bytes);
TB_API int tb_cell_fits(uint64_t tag, long long more);
__attribute__((noreturn)) TB_API void tb_cell_fall(const char *what, int line);
TB_API int tb_cell_can_fall(void);
TB_API int tb_mem_soft_now(void);
TB_API void tb_mem_soft(int d);
TB_API size_t tb_piece(void *p) { return (size_t)1 << (*(size_t *)((unsigned char *)p - 16) & 0xFF); }
// задачи и прерывания тоже берут память: на время выдачи прерывания выключены
TB_API void *malloc(size_t n) {
    uint64_t f = tb_irq_save();
    void *p = tb_malloc0(n);
    if (p) {
        uint64_t tag = tb_cell_charge(tb_piece(p));
        if (tag == (uint64_t)-1) {
            tb_free0(p);
            if (tb_mem_soft_now()) { tb_irq_restore(f); return 0; }
            tb_cell_fall("клетка взяла больше памяти, чем ей можно (квота 16 МБ)", 0);
        }
        ((uint64_t *)p)[-1] = tag;
    } else if (n && tb_cell_can_fall() && !tb_mem_soft_now()) tb_cell_fall("память ядра кончилась (запас оставлен службам)", 0);
    tb_irq_restore(f);
    return p;
}
TB_API void free(void *p) {
    if (p == 0) return;
    uint64_t f = tb_irq_save();
    tb_cell_refund(((uint64_t *)p)[-1], (long long)tb_piece(p));
    ((uint64_t *)p)[-1] = TB_TAG_FREE;   // свободный кусок ничей (его не «вернут» ещё раз)
    tb_free0(p);
    tb_irq_restore(f);
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
TB_API void *tb_realloc0(void *p, size_t n) {
    if (p == 0) return malloc(n);
    unsigned char *h = (unsigned char *)p - 16;
    int k = (int)(*(size_t *)h & 0xFF);
    size_t cap = (size_t)1 << k;
    if (n <= cap) return p;
    // последний кусок растёт на месте
    if (h + cap + 16 == tb_heap + tb_heap_top) {
        int k2 = tb_cls(n);
        size_t need = ((size_t)1 << k2) + 16;
        if ((size_t)(h - tb_heap) + need <= tb_heap_limit() && tb_cell_fits(((uint64_t *)p)[-1], (long long)(((size_t)1 << k2) - cap))) {
            *(size_t *)h = (size_t)k2 | TB_HMAGIC;
            tb_heap_top = (size_t)(h - tb_heap) + need;
            // кусок вырос на месте: хозяину — разница
            tb_cell_refund(((uint64_t *)p)[-1], -(long long)(((size_t)1 << k2) - cap));
            return p;
        }
    }
    void *q = malloc(n);
    if (q) { memcpy(q, p, cap); free(p); }
    return q;
}
TB_API void *realloc(void *p, size_t n) { uint64_t f = tb_irq_save(); void *q = tb_realloc0(p, n); tb_irq_restore(f); return q; }
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
TB_API int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b; }
    return 0;
}
TB_API char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
TB_API char *strstr(const char *h, const char *n) {
    size_t ln = strlen(n);
    if (ln == 0) return (char *)h;
    for (; *h; h++) if (*h == *n && strncmp(h, n, ln) == 0) return (char *)h;
    return 0;
}
TB_API long atol(const char *s) { long r = 0; while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0'); return r; }
TB_API double sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
TB_API double fabs(double x) { return x < 0 ? -x : x; }
TB_API double strtod(const char *s, char **end) {
    double r = 0, sign = 1, scale = 1;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { r = r * 10 + (*s++ - '0'); scale *= 10; } }
    if (end) *end = (char *)s;
    return sign * r / scale;
}
// маленький printf: %lld %ld %d %s %c %g (дробь — до 6 знаков после точки)
TB_API int tb_vfmt(char *out, size_t cap, const char *f, va_list ap) {
    size_t n = 0;
#define TB_O(ch) do { if (n + 1 < cap) out[n] = (ch); n++; } while (0)
    for (; *f; f++) {
        if (*f != '%') { TB_O(*f); continue; }
        f++;
        int ll = 0;
        while (*f == 'l') { ll++; f++; }
        if (*f == 's') { const char *s = va_arg(ap, const char *); while (*s) TB_O(*s++); }
        else if (*f == 'c') TB_O((char)va_arg(ap, int));
        else if (*f == 'x') {
            unsigned long long u = ll >= 2 ? va_arg(ap, unsigned long long) : ll == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            char d[24]; int k = 0;
            do { d[k++] = "0123456789abcdef"[u % 16]; u /= 16; } while (u);
            while (k) TB_O(d[--k]);
        }
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
TB_API int snprintf(char *out, size_t cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = tb_vfmt(out, cap, f, ap); va_end(ap); return n; }
TB_API int printf(const char *f, ...) { char b[512]; va_list ap; va_start(ap, f); int n = tb_vfmt(b, sizeof b, f, ap); va_end(ap); fputs(b, stdout); return n; }
TB_API int fprintf(FILE *o, const char *f, ...) { char b[512]; va_list ap; va_start(ap, f); int n = tb_vfmt(b, sizeof b, f, ap); va_end(ap); fputs(b, o); return n; }
// сортировка вставками (списки в ядре маленькие)
TB_API void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    unsigned char *a = base, tmp[64];
    if (size > sizeof tmp) return;
    for (size_t i = 1; i < n; i++) {
        memcpy(tmp, a + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(a + (j - 1) * size, tmp) > 0) { memcpy(a + j * size, a + (j - 1) * size, size); j--; }
        memcpy(a + j * size, tmp, size);
    }
}
// диска нет; файлы, которые программа открывает по готовому имени (load_font("fonts/x.ttf")),
// компилятор кладёт внутрь ядра (omnium/kernel/pack.py) — их и «открывает» fopen, только для чтения
typedef struct { const char *name; const unsigned char *data; long long n; } tb_pack;
extern tb_pack tb_packed[];
extern long long tb_npacked;
struct tb_file { const unsigned char *d; long long n, pos; };
TB_API FILE *fopen(const char *p, const char *m) {
    if (!p || !m || m[0] != 'r') return 0;
    for (long long i = 0; i < tb_npacked; i++)
        if (strcmp(tb_packed[i].name, p) == 0) {
            FILE *f = malloc(sizeof(FILE));
            if (!f) return 0;
            f->d = tb_packed[i].data; f->n = tb_packed[i].n; f->pos = 0;
            return f;
        }
    return 0;
}
TB_API int fclose(FILE *f) { if (f && f != stdout && f != stderr) free(f); return 0; }
TB_API size_t fread(void *b, size_t s, size_t n, FILE *f) {
    if (!f || f == stdout || f == stderr || s == 0) return 0;
    long long left = f->n - f->pos, want = (long long)(s * n);
    if (want > left) want = left - left % (long long)s;
    memcpy(b, f->d + f->pos, (size_t)want);
    f->pos += want;
    return (size_t)want / s;
}
TB_API int getchar(void) { return EOF; }
TB_API int system(const char *c) { (void)c; return -1; }
#define SEEK_SET 0
#define SEEK_END 2
TB_API int fseek(FILE *f, long o, int w) {
    if (!f || f == stdout || f == stderr) return -1;
    long long p = w == SEEK_END ? f->n + o : w == SEEK_SET ? o : f->pos + o;
    if (p < 0 || p > f->n) return -1;
    f->pos = p;
    return 0;
}
TB_API long ftell(FILE *f) { return f && f != stdout && f != stderr ? (long)f->pos : -1; }
TB_API int atexit(void (*fn)(void)) { (void)fn; return 0; }
typedef unsigned useconds_t;
TB_API void tb_wait_ms(long long n);
TB_API long long tb_uptime_ms(void);
TB_API int usleep(useconds_t us) { tb_wait_ms((us + 999) / 1000); return 0; }
TB_API int abs(int x) { return x < 0 ? -x : x; }
TB_API void *calloc(size_t n, size_t s) { void *p = malloc(n * s); if (p) memset(p, 0, n * s); return p; }
// своя маленькая математика (для рисования): точность double, без таблиц
TB_API double floor(double x) {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    double t = (double)(long long)x;
    return t > x ? t - 1 : t;
}
TB_API double ceil(double x) { return -floor(-x); }
TB_API double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }
TB_API double trunc(double x) { return x < 0 ? ceil(x) : floor(x); }
TB_API double fmod(double a, double b) { return b == 0 ? 0 : a - trunc(a / b) * b; }
TB_API float floorf(float x) { return (float)floor(x); }
TB_API float ceilf(float x) { return (float)ceil(x); }
TB_API float sqrtf(float x) { return (float)sqrt(x); }
TB_API float fabsf(float x) { return x < 0 ? -x : x; }
TB_API double sin(double x) {
    const double pi = 3.14159265358979323846;
    x = fmod(x, 2 * pi);
    if (x > pi) x -= 2 * pi;
    if (x < -pi) x += 2 * pi;
    if (x > pi / 2) x = pi - x;
    if (x < -pi / 2) x = -pi - x;
    double x2 = x * x, t = x, r = x;
    for (int k = 1; k < 12; k++) { t *= -x2 / ((2 * k) * (2 * k + 1)); r += t; }
    return r;
}
TB_API double cos(double x) { return sin(x + 1.57079632679489661923); }
TB_API double tan(double x) { double c = cos(x); return c == 0 ? 1e300 : sin(x) / c; }
TB_API float sinf(float x) { return (float)sin(x); }
TB_API float cosf(float x) { return (float)cos(x); }
TB_API double atan(double x) {
    if (x < 0) return -atan(-x);
    if (x > 1) return 1.57079632679489661923 - atan(1 / x);
    if (x > 0.2679491924311227) return 0.5235987755982988 + atan((x * 1.7320508075688772 - 1) / (x + 1.7320508075688772));
    double x2 = x * x, t = x, r = x;
    for (int k = 1; k < 14; k++) { t *= -x2; r += t / (2 * k + 1); }
    return r;
}
TB_API double atan2(double y, double x) {
    const double pi = 3.14159265358979323846;
    if (x > 0) return atan(y / x);
    if (x < 0) return y >= 0 ? atan(y / x) + pi : atan(y / x) - pi;
    return y > 0 ? pi / 2 : y < 0 ? -pi / 2 : 0;
}
TB_API double exp(double x) {
    if (x > 709) return 1e308;
    if (x < -745) return 0;
    long long k = (long long)floor(x / 0.6931471805599453 + 0.5);
    double r = x - (double)k * 0.6931471805599453, t = 1, s = 1;
    for (int i = 1; i < 18; i++) { t *= r / i; s += t; }
    union { double d; unsigned long long u; } v; v.d = s;
    v.u += (unsigned long long)k << 52;
    return v.d;
}
TB_API double log(double x) {
    if (x <= 0) return -1e308;
    union { double d; unsigned long long u; } v; v.d = x;
    long long e = (long long)((v.u >> 52) & 0x7FF) - 1023;
    v.u = (v.u & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;   // 1 <= m < 2
    double m = v.d;
    if (m > 1.4142135623730951) { m /= 2; e++; }
    double y = (m - 1) / (m + 1), y2 = y * y, t = y, r = 0;
    for (int k = 0; k < 20; k++) { r += t / (2 * k + 1); t *= y2; }
    return 2 * r + (double)e * 0.6931471805599453;
}
TB_API double pow(double a, double b) {
    if (b == 0) return 1;
    if (a == 0) return 0;
    if (b == (double)(long long)b && b > -64 && b < 64) {
        double r = 1, m = a; long long n = (long long)(b < 0 ? -b : b);
        while (n) { if (n & 1) r *= m; m *= m; n >>= 1; }
        return b < 0 ? 1 / r : r;
    }
    if (a < 0) return 0;
    return exp(b * log(a));
}
TB_API float powf(float a, float b) { return (float)pow(a, b); }
TB_API void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const unsigned char *p = (const unsigned char *)base + mid * size;
        int c = cmp(key, p);
        if (c == 0) return (void *)p;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}
TB_API long long tb_time_ms(void) { return tb_uptime_ms(); }
TB_API long long tb_time_us(void) { return tb_uptime_ms() * 1000; }
// ---------- ядро: прерывания, таймер, клавиатура, задачи, страницы памяти ----------
// on all cores здесь выполняется на одном ядре по очереди (как и было),
// а background — настоящая задача: таймер переключает задачи каждые 10 мс.
TB_API long sysconf(int n) { (void)n; return 1; }
TB_API int pthread_create(pthread_t *t, void *a, void *(*run)(void *), void *arg) { (void)t; (void)a; run(arg); return 0; }
TB_API int pthread_join(pthread_t t, void **r) { (void)t; (void)r; return 0; }

// адреса из раскладки ядра (компоновщик): где код, где кончается нижняя память, где куча
extern char tb_code_start[], tb_code_end[], tb_tls_start[], tb_tls_end[], tb_heap_area[], tb_kernel_end[];
TB_API unsigned char tb_stack[1 << 20] __attribute__((aligned(4096), used));
void tb_kmain(void);

// авария ядра: красным на экран и в порт, потом стоп
__attribute__((noreturn)) static void tb_panic(const char *what, uint64_t a, uint64_t b) {
    __asm__ volatile("cli");
    char buf[160];
    snprintf(buf, sizeof buf, "\n*** ядро остановлено: %s (0x%llx, 0x%llx)\n", what, (unsigned long long)a, (unsigned long long)b);
    fputs(buf, stderr);
    exit(3);
}

// --- таблица сегментов x86-64: код 64 бита, данные, задача процессора (TSS, 16 байт) ---
// у каждого ядра процессора свой TSS (две записи по 8 байт): номер сегмента 0x18 + 16 × ядро
TB_API uint64_t tb_gdt[3 + 2 * TB_NCPU] __attribute__((used)) = {0, 0x00AF9A000000FFFFULL, 0x00CF92000000FFFFULL};
// TSS: ist[0] — отдельный стек для двойной ошибки (стек упёрся в сторожа — обработчику есть куда писать)
typedef struct __attribute__((packed)) {
    uint32_t r0; uint64_t rsp[3]; uint64_t r1; uint64_t ist[7]; uint64_t r2; uint16_t r3, iomap;
} tb_tss64;
TB_API tb_tss64 tb_tsss[TB_NCPU] __attribute__((aligned(16)));
TB_API uint64_t tb_idt[2 * 256] __attribute__((aligned(16)));
TB_API struct __attribute__((packed)) { uint16_t lim; uint64_t base; } tb_gdtr, tb_idtr;

// 49 входов: 0..31 ошибки процессора, 32..47 железо (таймер, клавиатура ...), 48 — «уступить ход».
// Кадр на стеке (снизу вверх): 512 байт дробей и SSE (fxsave), 15 регистров, номер, код ошибки,
// и то, что положил процессор: rip, cs, rflags, rsp, ss. Задачи переключаются сменой кадра —
// у каждой задачи свои регистры и свои дроби лежат в её кадре.
extern char tb_isr_base[];
__asm__(".text\n.balign 16\n.global tb_isr_base\ntb_isr_base:\n"
        ".set tb_v, 0\n.rept 50\n.balign 16\n"
        ".if (tb_v == 8) || (tb_v == 10) || (tb_v == 11) || (tb_v == 12) || (tb_v == 13) || (tb_v == 14) || (tb_v == 17) || (tb_v == 21)\n"
        ".else\npushq $0\n.endif\npushq $tb_v\njmp tb_isr_common\n.set tb_v, tb_v + 1\n.endr\n"
        "tb_isr_common:\ncld\n"
        "pushq %rax\npushq %rcx\npushq %rdx\npushq %rbx\npushq %rbp\npushq %rsi\npushq %rdi\n"
        "pushq %r8\npushq %r9\npushq %r10\npushq %r11\npushq %r12\npushq %r13\npushq %r14\npushq %r15\n"
        "subq $512, %rsp\nfxsave (%rsp)\nmovq %rsp, %rdi\ncall tb_isr\nmovq %rax, %rsp\ncall tb_switch_done\nfxrstor (%rsp)\naddq $512, %rsp\n"
        "popq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %r11\npopq %r10\npopq %r9\npopq %r8\n"
        "popq %rdi\npopq %rsi\npopq %rbp\npopq %rbx\npopq %rdx\npopq %rcx\npopq %rax\n"
        "addq $16, %rsp\niretq\n");
#define TB_F_VEC 79
#define TB_F_ERR 80
#define TB_F_RIP 81
#define TB_FRAME (512 + 22 * 8)

// --- задачи: у каждой свой стек и свои «переменные потока» (temp, кэш длины) ---
#define TB_TLS_MAX 512
#define TB_NTASK 32
#define TB_TSTACK 65536
typedef struct { pthread_t th; volatile int done; void *res; } tb_task;
typedef struct {
    uint64_t *frame; int used; volatile int done;
    void *(*run)(void *); void *arg; tb_task *t;
    int on;                               // на каком ядре процессора идёт сейчас (-1 — ни на каком)
    int no;                               // номер клетки: по порядку запуска (1, 2, 3 ...)
    uint64_t gen; long long mem;          // поколение слота (для хозяина кусков) и байты клетки
    int *locks[8]; int nlocks;            // замки, которые клетка держит сейчас
    int svc;                              // служба (a = service f()): ей можно в запас памяти
    int soft;                             // сейчас new_mem / grow_mem: нет памяти — ошибка, а не падение
    unsigned char tls[TB_TLS_MAX + 16] __attribute__((aligned(16)));   // блок переменных потока + адрес самого себя
} tb_kslot;
TB_API tb_kslot tb_slots[TB_NTASK];          // 0 — main
TB_API unsigned char tb_tstacks[TB_NTASK][TB_TSTACK] __attribute__((aligned(4096)));
TB_API int tb_nlive = 1, tb_cell_count = 0;
// своё у каждой задачи (переменные потока): номер её слота; у функций прерывания tb_in_irq = 1
TB_API _Thread_local int tb_cur = 0;
TB_API _Thread_local int tb_in_irq = 0;
TB_API unsigned char tb_fpu_init[512] __attribute__((aligned(16)));
TB_API volatile uint64_t tb_ms = 0;
TB_API void (*tb_irq_fn[16])(void);
TB_API uint16_t tb_pic_mask = 0xFFFF;
TB_API uint64_t tb_tls_size(void) { return (uint64_t)(tb_tls_end - tb_tls_start); }
// блок переменных потока: начальные значения (образ от компоновщика), в конце — адрес конца
// (так устроено у x86-64: FS указывает на конец блока, а там лежит он сам)
TB_API unsigned char *tb_tls_fresh(unsigned char *b) {
    uint64_t n = tb_tls_size();
    memcpy(b, tb_tls_start, n);
    unsigned char *tp = b + n;
    *(uint64_t *)tp = (uint64_t)(uintptr_t)tp;
    return tp;
}
TB_API void tb_set_fs(unsigned char *tp) {
    uint64_t v = (uint64_t)(uintptr_t)tp;
    __asm__ volatile("wrmsr" : : "c"(0xC0000100u), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}
TB_API unsigned char *tb_tls_tp(unsigned char *b) { return b + tb_tls_size(); }
TB_INL void *tb_tls_self(void) { void *p; __asm__ volatile("movq %%fs:0, %0" : "=r"(p)); return p; }
// хозяин куска: номер слота и поколение; кусок клетки, которой уже нет, никому не засчитывается
TB_API uint64_t tb_cell_charge(size_t bytes) {
    if (tb_in_irq) return 0xFFFF;
    tb_kslot *s = &tb_slots[tb_cur];
    if (tb_cur != 0 && s->mem + (long long)bytes > TB_CELL_QUOTA) return (uint64_t)-1;
    s->mem += (long long)bytes;
    return (uint64_t)tb_cur | (s->gen << 8);
}
// new_mem / grow_mem просят память «мягко»: не хватило — ответ else err, клетка живёт
TB_API int tb_mem_soft_now(void) { return !tb_in_irq && tb_slots[tb_cur].soft > 0; }
TB_API void tb_mem_soft(int d) { if (!tb_in_irq) tb_slots[tb_cur].soft += d; }
TB_API size_t tb_heap_limit(void) {
    if (tb_in_irq || tb_cur == 0 || tb_slots[tb_cur].svc) return TB_HEAP;
    return TB_HEAP - TB_RESERVE;
}
// хватит ли клетке квоты, если её кусок вырастет на месте (иначе растём через malloc — он и уронит)
TB_API int tb_cell_fits(uint64_t tag, long long more) {
    uint64_t i = tag & 0xFF;
    if (i == 0 || i >= TB_NTASK || tb_slots[i].gen != (tag >> 8)) return 1;
    return tb_slots[i].mem + more <= TB_CELL_QUOTA;
}
TB_API void tb_cell_refund(uint64_t tag, long long bytes) {
    uint64_t i = tag & 0xFF;
    if (i >= TB_NTASK || tb_slots[i].gen != (tag >> 8) || !tb_slots[i].used) return;
    tb_slots[i].mem -= bytes;
}

TB_API void tb_pic_apply(void) { tb_outb(0x21, (unsigned char)tb_pic_mask); tb_outb(0xA1, (unsigned char)(tb_pic_mask >> 8)); }
TB_API void tb_irq_set(int v, void (*fn)(void)) {
    tb_irq_fn[v - 32] = fn;
    tb_pic_mask &= (uint16_t)~(1u << (v - 32));
    if (v >= 40) tb_pic_mask &= (uint16_t)~(1u << 2);
    tb_pic_apply();
}
TB_API void tb_irq_on(void) { __asm__ volatile("sti" ::: "memory"); }
TB_API void tb_irq_off(void) { __asm__ volatile("cli" ::: "memory"); }
TB_API void tb_yield(void) { if (!tb_in_irq) __asm__ volatile("int $48" ::: "memory"); }

// переключение задач: запомнить кадр и «переменные потока» текущей, отдать кадр следующей
TB_API uint64_t *tb_schedule(uint64_t *frame) {
    tb_cpu *c = tb_me();
    uint64_t f = tb_irq_save();
    int cur = c->run;
    if (cur >= 0) tb_slots[cur].frame = frame; else c->idle_frame = frame;
    // следующая по кругу: живая и не идущая на другом ядре
    int n = -1;
    for (int k = 1; k <= TB_NTASK; k++) {
        int j = ((cur < 0 ? 0 : cur) + k) % TB_NTASK;
        tb_kslot *s = &tb_slots[j];
        if (s->used && !s->done && (s->on < 0 || j == cur)) { n = j; break; }
    }
    // прежнюю задачу отдаём другим ядрам только после смены стека (tb_switch_done):
    // пока мы в этой функции, мы ещё на её стеке
    c->prev = (cur >= 0 && n != cur) ? cur : -1;
    uint64_t *next;
    if (n >= 0) {
        tb_slots[n].on = c->id;
        c->run = n;
        tb_set_fs(tb_tls_tp(tb_slots[n].tls));
        next = tb_slots[n].frame;
    } else {
        c->run = -1;
        tb_set_fs(tb_tls_tp(c->irq_tls));
        next = c->idle_frame;
    }
    c->slice = 10;
    tb_irq_restore(f);
    return next;
}
// стек уже новый: прежнюю задачу можно брать другим ядрам, а кончившуюся — освободить
__attribute__((used)) TB_API void tb_switch_done(void) {
    tb_cpu *c = tb_me();
    int p = c->prev;
    if (p < 0) return;
    c->prev = -1;
    uint64_t f = tb_irq_save();
    if (tb_slots[p].done && p != 0) { tb_slots[p].used = 0; tb_nlive--; }
    tb_slots[p].on = -1;
    tb_irq_restore(f);
}
TB_API void tb_task_entry(void) {
    tb_kslot *s = &tb_slots[tb_cur];
    s->run(s->arg);
    __asm__ volatile("cli");
    s->t->done = 1;
    s->done = 1;
    tb_bkl_drop();
    for (;;) __asm__ volatile("int $48");
}
static void tb_task_start2(tb_task *t, void *(*run)(void *), void *arg, int svc);
TB_API void tb_task_start(tb_task *t, void *(*run)(void *), void *arg) { tb_task_start2(t, run, arg, 0); }
TB_API void tb_service_start(tb_task *t, void *(*run)(void *), void *arg) { tb_task_start2(t, run, arg, 1); }
static void tb_task_start2(tb_task *t, void *(*run)(void *), void *arg, int svc) {
    uint64_t f = tb_irq_save();
    int i = 1;
    while (i < TB_NTASK && tb_slots[i].used) i++;
    if (i == TB_NTASK) tb_panic("больше 31 задачи background сразу", 0, 0);
    tb_kslot *s = &tb_slots[i];
    s->used = 1; s->done = 0; s->run = run; s->arg = arg; s->t = t;
    s->gen++; s->mem = 0; s->nlocks = 0; s->on = -1; s->no = ++tb_cell_count; s->svc = svc; s->soft = 0;
    t->done = 0; t->th = i;
    tb_tls_fresh(s->tls);
    *(int *)(tb_tls_tp(s->tls) - ((unsigned char *)tb_tls_self() - (unsigned char *)&tb_cur)) = i;
    // стек задачи выглядит так, будто её прервали прямо перед tb_task_entry
    unsigned char *top = tb_tstacks[i] + TB_TSTACK;
    uint64_t *fr = (uint64_t *)(top - TB_FRAME);
    memset(fr, 0, TB_FRAME);
    memcpy(fr, tb_fpu_init, 512);
    fr[TB_F_RIP] = (uint64_t)(uintptr_t)tb_task_entry;
    fr[TB_F_RIP + 1] = 0x08;                       // cs
    fr[TB_F_RIP + 2] = 0x202;                      // rflags: прерывания включены
    fr[TB_F_RIP + 3] = (uint64_t)(uintptr_t)(top - 8);   // rsp: как после call
    fr[TB_F_RIP + 4] = 0x10;                       // ss
    s->frame = fr;
    tb_nlive++;
    tb_irq_restore(f);
}
// a.crashed: задача упала (в ядре клетка падает одна; в обычной программе падает вся программа)
TB_API _Bool tb_crashed(tb_task *t) {
    while (!t->done) tb_yield();
    return t->done == 2;
}
// a.wait: пока задача работает, отдаём ход другим
TB_API void *tb_wait(tb_task *t) {
    if (tb_in_irq && !t->done) tb_panic("wait внутри прерывания нельзя", 0, 0);
    while (!t->done) tb_yield();
    return t->res;
}

// --- время ---
TB_API long long tb_uptime_ms(void) { uint64_t f = tb_irq_save(); uint64_t v = tb_ms; tb_irq_restore(f); return (long long)v; }
TB_API void tb_wait_ms(long long n) {
    if (tb_in_irq) tb_panic("wait_ms внутри прерывания нельзя: прерывание должно быть коротким", 0, 0);
    long long end = tb_uptime_ms() + n;
    while (tb_uptime_ms() < end) {
        if (tb_nlive > 1) tb_yield();
        else __asm__ volatile("sti; hlt" ::: "memory");
    }
}

// --- клавиатура: буквы копятся в очереди, read_key() берёт по одной ---
TB_API const char tb_kmap[2][58] = {
    {0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
     'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
     'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
     'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
    {0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
     'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
     'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
     'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '}};
TB_API char tb_kq[256];
TB_API volatile unsigned char tb_khead = 0, tb_ktail = 0;
TB_API int tb_kshift = 0;
TB_API char tb_ktext[256][2];
// окно открыто (window_open): клавиши идут и сюда — для key_down и typed_text
TB_API void (*tb_key_hook)(unsigned char sc) = 0;
TB_API void tb_key_byte(unsigned char sc) {
    if (tb_key_hook) tb_key_hook(sc);
    if (sc == 0x2A || sc == 0x36) { tb_kshift = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { tb_kshift = 0; return; }
    if (sc >= 58) return;
    char ch = tb_kmap[tb_kshift][sc];
    if (ch == 0 || (unsigned char)(tb_khead + 1) == tb_ktail) return;
    tb_kq[tb_khead] = ch;
    tb_khead = (unsigned char)(tb_khead + 1);
}
TB_API void tb_key_irq(void) {
    unsigned char st = tb_inb(0x64);
    if (!(st & 1)) return;
    unsigned char b = tb_inb(0x60);
    if (st & 0x20) return;   // байт мыши (его ждёт прерывание 12)
    tb_key_byte(b);
}
TB_API const char *tb_read_key(void) {
    uint64_t f = tb_irq_save();
    const char *r = "";
    if (tb_ktail != tb_khead) { r = tb_ktext[(unsigned char)tb_kq[tb_ktail]]; tb_ktail = (unsigned char)(tb_ktail + 1); }
    tb_irq_restore(f);
    return r;
}

// --- страницы памяти (4 уровня): нижняя память — страницы по 4 КБ (адрес 0 и код защищены,
// у стеков — пустая страница-сторож), выше — большие по 2 МБ (куча и свободная память) ---
#define TB_LOW_PTS 32                                 // 32 таблицы × 2 МБ = нижние 64 МБ
#define TB_MAX_GB 64                                  // память до 64 ГБ
TB_API uint64_t tb_pml4[512] __attribute__((aligned(4096)));
TB_API uint64_t tb_pdpt[512] __attribute__((aligned(4096)));
TB_API uint64_t tb_pdir[TB_MAX_GB][512] __attribute__((aligned(4096)));
TB_API uint64_t tb_pt[TB_LOW_PTS][512] __attribute__((aligned(4096)));
TB_API uint64_t tb_ram_end = 0, tb_pg_next = 0, tb_pg_free = 0;
// таблицы на время загрузки: первые 4 ГБ большими страницами (их строит _start ещё в 32 битах)
TB_API uint64_t tb_boot_pml4[512] __attribute__((aligned(4096), used));
TB_API uint64_t tb_boot_pdpt[512] __attribute__((aligned(4096), used));
TB_API uint64_t tb_boot_pd[4][512] __attribute__((aligned(4096), used));
TB_API uint32_t tb_boot_magic __attribute__((used)), tb_boot_mbi __attribute__((used));
TB_API void tb_unmap(uint64_t a) { tb_pt[a >> 21][(a >> 12) & 511] = 0; }
TB_API int tb_is_guard(uint64_t a) {
    if (a >= (uint64_t)(uintptr_t)tb_stack && a < (uint64_t)(uintptr_t)tb_stack + 4096) return 1;
    for (int i = 1; i < TB_NTASK; i++)
        if (a >= (uint64_t)(uintptr_t)tb_tstacks[i] && a < (uint64_t)(uintptr_t)tb_tstacks[i] + 4096) return 1;
    return 0;
}
TB_API void tb_paging_init(void) {
    uint64_t low = (uint64_t)(uintptr_t)tb_heap_area;
    uint64_t cs = (uint64_t)(uintptr_t)tb_code_start, ce = (uint64_t)(uintptr_t)tb_code_end;
    if (low > TB_LOW_PTS * 0x200000ull) tb_panic("ядро слишком большое для нижних страниц", low, 0);
    if (tb_ram_end < (uint64_t)(uintptr_t)tb_kernel_end + 0x400000u) tb_panic("мало памяти: запусти qemu с -m 256", tb_ram_end, (uint64_t)(uintptr_t)tb_kernel_end);
    for (uint64_t a = 0; a < low; a += 4096) {
        uint64_t flags = 3;                                   // есть, можно писать
        if (a >= cs && a < ce) flags = 1;                     // код: только читать
        tb_pt[a >> 21][(a >> 12) & 511] = a | flags;
    }
    tb_unmap(0);                                              // адрес 0: «пустая ссылка»
    tb_unmap((uint64_t)(uintptr_t)tb_stack);                  // сторож под стеком main
    for (int i = 1; i < TB_NTASK; i++) tb_unmap((uint64_t)(uintptr_t)tb_tstacks[i]);
    tb_pml4[0] = (uint64_t)(uintptr_t)tb_pdpt | 3;
    uint64_t gbs = (tb_ram_end + (1ull << 30) - 1) >> 30;
    if (gbs > TB_MAX_GB) gbs = TB_MAX_GB;
    if (gbs < 1) gbs = 1;
    for (uint64_t g = 0; g < gbs; g++) tb_pdpt[g] = (uint64_t)(uintptr_t)tb_pdir[g] | 3;
    for (uint64_t t = 0; t < low >> 21; t++) tb_pdir[0][t] = (uint64_t)(uintptr_t)tb_pt[t] | 3;
    for (uint64_t a = low; a < tb_ram_end; a += 0x200000ull) tb_pdir[a >> 30][(a >> 21) & 511] = a | 0x83;
    // APIC: окно устройства, без кэша
    tb_pdpt[3] = (uint64_t)(uintptr_t)tb_pdir[3] | 3;
    tb_pdir[3][(0xFEE00000ull >> 21) & 511] = 0xFEE00000ull | 0x83 | 0x18;
    // CR0.WP: и ядро не может писать в страницы «только читать»
    __asm__ volatile("movq %0, %%cr3\nmovq %%cr0, %%rax\norq $0x10000, %%rax\nmovq %%rax, %%cr0\n" : : "r"(tb_pml4) : "rax", "memory");
    tb_pg_next = (uint64_t)(uintptr_t)tb_kernel_end;
}
TB_API long long tb_page_new(void) {
    uint64_t f = tb_irq_save(), p = 0;
    if (tb_pg_free) { p = tb_pg_free; tb_pg_free = *(uint64_t *)(uintptr_t)p; }
    else if (tb_pg_next + 4096 <= tb_ram_end) { p = tb_pg_next; tb_pg_next += 4096; }
    tb_irq_restore(f);
    if (p == 0) tb_panic("свободные страницы памяти кончились", tb_ram_end, 0);
    memset((void *)(uintptr_t)p, 0, 4096);
    return (long long)p;
}
TB_API void tb_page_free(long long a) {
    uint64_t p = (uint64_t)a;
    if ((p & 4095) || p < (uint64_t)(uintptr_t)tb_kernel_end || p >= tb_pg_next) tb_panic("page_free: это не страница из page_new", p, 0);
    uint64_t f = tb_irq_save();
    *(uint64_t *)(uintptr_t)p = tb_pg_free;
    tb_pg_free = p;
    tb_irq_restore(f);
}

// --- APIC: контроллер прерываний у каждого ядра процессора (по адресу 0xFEE00000) ---
TB_API volatile uint32_t *tb_lapic = (volatile uint32_t *)0xFEE00000;
TB_API uint32_t tb_lapic_ms = 0;            // сколько тактов таймера APIC в 1 мс (меряем по PIT)
TB_INL void tb_lapic_w(int r, uint32_t v) { tb_lapic[r / 4] = v; }
TB_INL uint32_t tb_lapic_r(int r) { return tb_lapic[r / 4]; }
// --- ошибки процессора ---
TB_API const char *tb_fault_name[32] = {"деление на ноль", "отладка", "NMI", "точка останова", "переполнение",
    "выход за границы", "неизвестная команда", "нет FPU", "двойная ошибка", "", "плохой TSS", "нет сегмента",
    "ошибка стека", "нарушение защиты", "ошибка страницы", "", "ошибка дробей", "выравнивание", "проверка железа", "ошибка SIMD"};
// клетка (задача background) упала: только она и заканчивается, ядро и другие клетки живут.
// Тот, кто её запустил, узнаёт это из .crashed (done == 2). Main и прерывание так не умеют:
// их падение останавливает ядро
TB_API int tb_cell_can_fall(void) { return !tb_in_irq && tb_cur != 0; }
// кто пишет строку: номер задачи + 1 (прерывание — -1, оно не ждёт)
TB_API long long tb_out_me(void) { return tb_in_irq ? -1 : (long long)tb_cur + 1; }
// --- владение памятью (обменная куча, как в старой Омниум): у куска один хозяин — клетка.
// Что уходит из клетки (аргументы background, текст в канале, список через move), становится
// ничьим (метка 0, «ядро») или переходит получателю. Упала клетка — ядро возвращает всё, что
// у неё осталось: этого больше никто не видит.
TB_API int tb_is_piece(const void *p) {
    const unsigned char *u = p;
    if (!p || ((uintptr_t)p & 15) || u < tb_heap + 16 || u >= tb_heap + tb_heap_top) return 0;
    size_t w = *(const size_t *)(u - 16);
    return (w & ~(size_t)0xFF) == TB_HMAGIC && (w & 0xFF) >= 4 && (w & 0xFF) < 32 && ((const uint64_t *)p)[-1] != TB_TAG_FREE;
}
// кусок становится ничьим (его хозяин больше не платит за него и не утащит при падении)
TB_API void tb_export(const void *p) {
    uint64_t f = tb_irq_save();
    if (tb_is_piece(p)) {
        uint64_t *tag = &((uint64_t *)p)[-1];
        tb_cell_refund(*tag, (long long)tb_piece((void *)p));
        *tag = 0;
    }
    tb_irq_restore(f);
}
// кусок переходит текущей клетке (взяла из канала)
TB_API void tb_adopt(const void *p) {
    uint64_t f = tb_irq_save();
    if (tb_is_piece(p)) {
        uint64_t *tag = &((uint64_t *)p)[-1];
        tb_cell_refund(*tag, (long long)tb_piece((void *)p));
        uint64_t t = tb_cell_charge(tb_piece((void *)p));
        *tag = t == (uint64_t)-1 ? 0 : t;   // сверх квоты — пусть будет ничьим, клетку не роняем на приёме
    }
    tb_irq_restore(f);
}
// упала клетка: вернуть все куски с её меткой
TB_API long long tb_cell_reclaim(uint64_t tag) {
    long long got = 0;
    if ((tag & 0xFF) == 0) return 0;
    uint64_t f = tb_irq_save();
    size_t at = 0;
    while (at < tb_heap_top) {
        unsigned char *h = tb_heap + at;
        size_t k = *(size_t *)h & 0xFF;
        size_t next = at + ((size_t)1 << k) + 16;
        uint64_t *p = (uint64_t *)(h + 16);
        if (p[-1] == tag) {
            got += (long long)((size_t)1 << k);
            p[-1] = TB_TAG_FREE;
            tb_free0(p);
        }
        at = next;
    }
    tb_irq_restore(f);
    return got;
}
TB_API void tb_cell_mark(const char *what, int line) {
    tb_kslot *s = &tb_slots[tb_cur];
    // замки, которые клетка держала, отпускаем: иначе на них застрянут другие
    for (int i = 0; i < s->nlocks; i++) __atomic_store_n(s->locks[i], 0, __ATOMIC_RELEASE);
    s->nlocks = 0;
    char buf[200];
    if (line > 0) snprintf(buf, sizeof buf, "*** клетка %d упала: строка %d: %s\n", s->no, line, what);
    else snprintf(buf, sizeof buf, "*** клетка %d упала: %s\n", s->no, what);
    fputs(buf, stderr);
    tb_cell_reclaim((uint64_t)tb_cur | (s->gen << 8));
    s->mem = 0;
    s->t->done = 2;
    s->done = 1;
}
__attribute__((noreturn)) TB_API void tb_cell_fall(const char *what, int line) {
    __asm__ volatile("cli");
    tb_cell_mark(what, line);
    tb_bkl_drop();
    for (;;) __asm__ volatile("int $48");
}
TB_API uint64_t *tb_fault(uint64_t v, uint64_t *f) {
    uint64_t rip = f[TB_F_RIP], a;
    const char *what;
    __asm__ volatile("movq %%cr2, %0" : "=r"(a));
    if ((v == 14 || v == 8) && tb_is_guard(a)) what = "стек переполнен: слишком глубокая рекурсия";
    else if (v == 14 && a < 4096) what = "обращение по пустому адресу (адрес 0)";
    else if (v == 14 && (f[TB_F_ERR] & 2) && a >= (uint64_t)(uintptr_t)tb_code_start && a < (uint64_t)(uintptr_t)tb_code_end) what = "запись в код программы";
    else if (v == 14) what = "обращение к памяти, которой нет";
    else if (v == 8) what = "двойная ошибка процессора";
    else what = v < 20 && tb_fault_name[v][0] ? tb_fault_name[v] : "ошибка процессора";
    if (tb_cell_can_fall()) {
        tb_cell_mark(what, 0);
        tb_bkl_drop();
        return tb_schedule(f);
    }
    tb_panic(what, v == 14 || v == 8 ? a : v, rip);
}

// все прерывания приходят сюда; ответ — кадр, в который вернуться (так переключаются задачи)
__attribute__((used)) static uint64_t *tb_isr(uint64_t *f) {
    uint64_t v = f[TB_F_VEC];
    if (v < 32) return tb_fault(v, f);
    if (v == 48) return tb_schedule(f);
    if (v == 49) {
        // таймер своего ядра процессора (APIC): 1000 раз в секунду
        tb_lapic_w(0xB0, 0);
        if (--tb_me()->slice <= 0) return tb_schedule(f);
        return f;
    }
    int irq = (int)v - 32;
    if (irq == 0) tb_ms++;
    if (irq == 1) tb_key_irq();
    if (tb_irq_fn[irq]) {
        // своя функция прерывания: со своими «переменными потока»
        tb_cpu *c = tb_me();
        tb_set_fs(tb_tls_tp(c->irq_tls));
        tb_irq_fn[irq]();
        tb_set_fs(tb_tls_tp(c->run >= 0 ? tb_slots[c->run].tls : c->irq_tls));
    }
    if (irq >= 8) tb_outb(0xA0, 0x20);
    tb_outb(0x20, 0x20);
    if (irq == 0 && --tb_me()->slice <= 0) return tb_schedule(f);
    return f;
}

TB_API void tb_idt_set(int v, uint64_t a, int ist) {
    tb_idt[2 * v] = (a & 0xFFFFu) | (0x08ull << 16) | ((uint64_t)ist << 32) | (0x8Eull << 40) | (((a >> 16) & 0xFFFFu) << 48);
    tb_idt[2 * v + 1] = a >> 32;
}

// загрузка: заголовок multiboot (флаг 2: «скажи, сколько памяти»). _start работает ещё в 32 битах:
// строит таблицы страниц на первые 4 ГБ, включает режим 64 бит и прыгает в tb_kstart
__attribute__((section(".multiboot"), used, aligned(4)))
TB_API const unsigned int tb_multiboot[3] = {0x1BADB002u, 2u, (unsigned int)-(0x1BADB002 + 2)};
// TSS ядра процессора i: ist1 — его стек для двойной ошибки
TB_API void tb_tss_make(int i) {
    tb_tss64 *t = &tb_tsss[i];
    t->ist[0] = (uint64_t)(uintptr_t)(tb_cpus[i].df_stack + sizeof tb_cpus[i].df_stack);
    t->iomap = sizeof *t;
    uint64_t a = (uint64_t)(uintptr_t)t;
    tb_gdt[3 + 2 * i] = (sizeof *t - 1) | ((a & 0xFFFFFFull) << 16) | (0x89ull << 40) | (((a >> 24) & 0xFF) << 56);
    tb_gdt[4 + 2 * i] = a >> 32;
}
TB_API void tb_set_gs(tb_cpu *c) {
    uint64_t v = (uint64_t)(uintptr_t)c;
    __asm__ volatile("wrmsr" : : "c"(0xC0000101u), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}
// пустой ход ядра процессора: ждать прерывания
__attribute__((noreturn)) TB_API void tb_idle(void) { for (;;) __asm__ volatile("sti\nhlt"); }
// запись ядра процессора: стек и кадр пустого хода, переменные потока для прерываний
TB_API void tb_cpu_init(tb_cpu *c, int id) {
    c->self = c; c->id = id; c->run = -1; c->slice = 10; c->prev = -1;
    unsigned char *tp = tb_tls_fresh(c->irq_tls);
    *(int *)(tp - ((unsigned char *)tb_tls_self() - (unsigned char *)&tb_in_irq)) = 1;
    unsigned char *top = c->idle_stack + sizeof c->idle_stack;
    uint64_t *fr = (uint64_t *)(top - TB_FRAME);
    memset(fr, 0, TB_FRAME);
    memcpy(fr, tb_fpu_init, 512);
    fr[TB_F_RIP] = (uint64_t)(uintptr_t)tb_idle;
    fr[TB_F_RIP + 1] = 0x08; fr[TB_F_RIP + 2] = 0x202;
    fr[TB_F_RIP + 3] = (uint64_t)(uintptr_t)(top - 8); fr[TB_F_RIP + 4] = 0x10;
    c->idle_frame = fr;
}

// --- несколько ядер процессора («звёзды») ---
// Какие ядра есть — из таблицы ACPI «APIC» (MADT). Каждое будим так: INIT, потом дважды
// SIPI с адресом 0x8000; там маленький код (ниже) переводит ядро из 16 бит сразу в 64
// и прыгает в tb_ap_main со своим стеком.
TB_API int tb_apic_ids[TB_NCPU];
// таблицы ACPI лежат выше памяти, которую назвал загрузчик: показываем их кусок 2 МБ (только читать)
TB_API void tb_map_2m(uint64_t a) {
    if (a >= (4ull << 30) || a < (uint64_t)(uintptr_t)tb_heap_area) return;
    uint64_t *e = &tb_pdir[a >> 30][(a >> 21) & 511];
    if (*e) return;
    tb_pdpt[a >> 30] = (uint64_t)(uintptr_t)tb_pdir[a >> 30] | 3;
    *e = (a & ~0x1FFFFFull) | 0x81;
    __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
}
TB_API int tb_find_cpus(void) {
    unsigned char *rsdp = 0;
    for (uint64_t a = 0xE0000; a < 0x100000 && !rsdp; a += 16)
        if (memcmp((void *)(uintptr_t)a, "RSD PTR ", 8) == 0) rsdp = (unsigned char *)(uintptr_t)a;
    if (!rsdp) return 0;
    unsigned char *rsdt = (unsigned char *)(uintptr_t)*(uint32_t *)(rsdp + 16);
    tb_map_2m((uint64_t)(uintptr_t)rsdt);
    uint32_t len = *(uint32_t *)(rsdt + 4);
    int n = 0;
    for (uint32_t o = 36; o + 4 <= len; o += 4) {
        unsigned char *t = (unsigned char *)(uintptr_t)*(uint32_t *)(rsdt + o);
        tb_map_2m((uint64_t)(uintptr_t)t);
        if (memcmp(t, "APIC", 4) != 0) continue;
        uint32_t tl = *(uint32_t *)(t + 4);
        for (uint32_t e = 44; e + 2 <= tl && n < TB_NCPU; e += t[e + 1]) {
            if (t[e + 1] < 2) break;
            if (t[e] == 0 && (t[e + 4] & 1)) tb_apic_ids[n++] = t[e + 3];
        }
    }
    return n;
}
// код пробуждения: копируется в 0x8000; адреса внутри считаются от 0x8000
extern char tb_tramp[], tb_tramp_end[];
TB_API volatile uint64_t tb_ap_stack = 0, tb_ap_cr3 = 0;
TB_API volatile int tb_ap_up = 0, tb_ap_id = 0;
__asm__(".text\n.global tb_tramp\n.global tb_tramp_end\n.global tb_tr_cr3\n.global tb_tr_stack\n.code16\ntb_tramp:\n"
        "  cli\n  xorw %ax, %ax\n  movw %ax, %ds\n"
        "  lgdtl 0x8000 + (tb_tr_gdtr - tb_tramp)\n"
        "  movl %cr0, %eax\n  orl $1, %eax\n  movl %eax, %cr0\n"
        "  ljmpl $0x08, $(0x8000 + (tb_tr32 - tb_tramp))\n"
        ".code32\ntb_tr32:\n"
        "  movw $0x10, %ax\n  movw %ax, %ds\n  movw %ax, %ss\n"
        "  movl %cr4, %eax\n  orl $0x620, %eax\n  movl %eax, %cr4\n"
        "  movl 0x8000 + (tb_tr_cr3 - tb_tramp), %eax\n  movl %eax, %cr3\n"
        "  movl $0xC0000080, %ecx\n  rdmsr\n  orl $0x100, %eax\n  wrmsr\n"
        "  movl %cr0, %eax\n  andl $0xFFFFFFFB, %eax\n  orl $0x80010002, %eax\n  movl %eax, %cr0\n"
        "  ljmpl $0x18, $(0x8000 + (tb_tr64 - tb_tramp))\n"
        ".code64\ntb_tr64:\n"
        "  movq 0x8000 + (tb_tr_stack - tb_tramp), %rsp\n"
        "  movabsq $tb_ap_main, %rax\n  call *%rax\n"
        "1:\n  cli\n  hlt\n  jmp 1b\n"
        ".balign 8\ntb_tr_gdt:\n  .quad 0, 0x00CF9A000000FFFF, 0x00CF92000000FFFF, 0x00AF9A000000FFFF\n"
        "tb_tr_gdtr:\n  .word 31\n  .long 0x8000 + (tb_tr_gdt - tb_tramp)\n"
        ".balign 8\ntb_tr_cr3:\n  .quad 0\ntb_tr_stack:\n  .quad 0\n"
        "tb_tramp_end:\n");
TB_API void tb_lapic_timer_start(void) {
    tb_lapic_w(0x3E0, 3);                              // делитель 16
    tb_lapic_w(0x320, 49 | 0x20000);                   // вектор 49, по кругу
    tb_lapic_w(0x380, tb_lapic_ms);
}
TB_API void tb_load_tables(int i) {
    tb_gdtr.lim = sizeof tb_gdt - 1; tb_gdtr.base = (uint64_t)(uintptr_t)tb_gdt;
    __asm__ volatile("lgdt %0\n"
                     "pushq $0x08\nleaq 1f(%%rip), %%rax\npushq %%rax\nlretq\n1:\n"
                     "movw $0x10, %%ax\nmovw %%ax, %%ds\nmovw %%ax, %%es\nmovw %%ax, %%fs\nmovw %%ax, %%gs\nmovw %%ax, %%ss\n"
                     : : "m"(tb_gdtr) : "rax", "memory");
    __asm__ volatile("ltr %w0" : : "r"(0x18 + 16 * i));
    __asm__ volatile("lidt %0" : : "m"(tb_idtr));
}
// ядро процессора проснулось: свои сегменты, своя запись, таймер APIC — и в пустой ход
__attribute__((used, noreturn)) TB_API void tb_ap_main(void) {
    int i = tb_ap_id;
    tb_cpu *c = &tb_cpus[i];
    c->self = c;
    tb_set_gs(c);
    __asm__ volatile("fninit");
    tb_load_tables(i);
    tb_set_gs(c);
    tb_set_fs(tb_tls_tp(c->irq_tls));
    tb_lapic_w(0xF0, 0x100 | 48);                      // APIC включён; ложные прерывания — как «уступить ход»
    tb_lapic_timer_start();
    __atomic_store_n(&tb_ap_up, 1, __ATOMIC_RELEASE);
    tb_idle();
}
TB_API void tb_spin_ms(uint64_t n) { uint64_t end = tb_ms + n; while (tb_ms < end) __asm__ volatile("pause"); }
TB_API void tb_smp_start(void) {
    // такты APIC за 10 мс (по таймеру PIT, он уже идёт)
    tb_lapic_w(0xF0, 0x100 | 48);
    tb_lapic_w(0x3E0, 3);
    tb_lapic_w(0x320, 0x10000 | 49);                   // пока без прерывания
    uint64_t t0 = tb_ms; while (tb_ms == t0) __asm__ volatile("pause");
    tb_lapic_w(0x380, 0xFFFFFFFFu);
    tb_spin_ms(10);
    tb_lapic_ms = (0xFFFFFFFFu - tb_lapic_r(0x390)) / 10;
    tb_lapic_w(0x380, 0);
    int n = tb_find_cpus();
    int me = (int)(tb_lapic_r(0x20) >> 24);
    tb_cpus[0].apic = me;
    memcpy((void *)0x8000, tb_tramp, (size_t)(tb_tramp_end - tb_tramp));
    for (int k = 0; k < n && tb_ncpu < TB_NCPU; k++) {
        if (tb_apic_ids[k] == me) continue;
        int i = tb_ncpu;
        tb_cpu *c = &tb_cpus[i];
        tb_cpu_init(c, i);
        c->apic = tb_apic_ids[k];
        tb_tss_make(i);
        uint64_t cr3; __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
        char *base = (char *)0x8000;
        extern char tb_tr_cr3[], tb_tr_stack[];
        *(volatile uint64_t *)(base + (tb_tr_cr3 - tb_tramp)) = cr3;
        *(volatile uint64_t *)(base + (tb_tr_stack - tb_tramp)) = (uint64_t)(uintptr_t)(c->idle_stack + sizeof c->idle_stack - 64);
        tb_ap_id = i;
        tb_ap_up = 0;
        tb_lapic_w(0x310, (uint32_t)c->apic << 24); tb_lapic_w(0x300, 0x4500);        // INIT
        tb_spin_ms(10);
        for (int r = 0; r < 2 && !tb_ap_up; r++) {
            tb_lapic_w(0x310, (uint32_t)c->apic << 24); tb_lapic_w(0x300, 0x4600 | (0x8000 >> 12));   // SIPI
            tb_spin_ms(2);
        }
        uint64_t end = tb_ms + 200;
        while (!__atomic_load_n(&tb_ap_up, __ATOMIC_ACQUIRE) && tb_ms < end) __asm__ volatile("pause");
        if (tb_ap_up) tb_ncpu++;
    }
}
__attribute__((used)) static void tb_kstart(void) {
    tb_cpus[0].self = &tb_cpus[0];
    tb_set_gs(&tb_cpus[0]);
    uint32_t magic = tb_boot_magic;
    uint32_t *mbi = (uint32_t *)(uintptr_t)tb_boot_mbi;
    __asm__ volatile("fninit");
    __asm__ volatile("fxsave %0" : "=m"(tb_fpu_init));
    // COM1: 115200 бод, 8 бит
    tb_outb(0x3F8 + 1, 0x00); tb_outb(0x3F8 + 3, 0x80); tb_outb(0x3F8 + 0, 0x01);
    tb_outb(0x3F8 + 1, 0x00); tb_outb(0x3F8 + 3, 0x03); tb_outb(0x3F8 + 2, 0xC7);
    tb_vga_font();
    for (int i = 0; i < 80 * 25; i++) tb_vga[i] = (unsigned short)(tb_vcolor << 8 | ' ');
    tb_outb(0x3D4, 0x0A); tb_outb(0x3D5, 0x20);  // мигающий курсор не нужен
    tb_ram_end = 128u << 20;
    if (magic == 0x2BADB002u && (mbi[0] & 1)) tb_ram_end = (1ull << 20) + (uint64_t)mbi[2] * 1024u;
    if (tb_ram_end < (1u << 20)) tb_ram_end = 128u << 20;
    if (tb_tls_size() > TB_TLS_MAX) tb_panic("переменных потока больше, чем место для них", tb_tls_size(), 0);
    for (int i = 0; i < 256; i++) { tb_ktext[i][0] = (char)i; tb_ktext[i][1] = 0; }
    // сегменты и TSS (ist1 — стек для двойной ошибки)
    tb_tss_make(0);
    tb_gdtr.lim = sizeof tb_gdt - 1; tb_gdtr.base = (uint64_t)(uintptr_t)tb_gdt;
    __asm__ volatile("lgdt %0\n"
                     "pushq $0x08\nleaq 1f(%%rip), %%rax\npushq %%rax\nlretq\n1:\n"
                     "movw $0x10, %%ax\nmovw %%ax, %%ds\nmovw %%ax, %%es\nmovw %%ax, %%fs\nmovw %%ax, %%gs\nmovw %%ax, %%ss\n"
                     : : "m"(tb_gdtr) : "rax", "memory");
    __asm__ volatile("ltr %w0" : : "r"(0x18));
    // загрузка сегментов обнуляет базы FS и GS: ставим заново. Переменные потока main — свои
    tb_set_gs(&tb_cpus[0]);
    tb_set_fs(tb_tls_fresh(tb_slots[0].tls));
    tb_cpu_init(&tb_cpus[0], 0);
    tb_cpus[0].run = 0;
    // прерывания
    for (int v = 0; v < 50; v++) tb_idt_set(v, (uint64_t)(uintptr_t)tb_isr_base + 16u * (uint64_t)v, v == 8 ? 1 : 0);
    tb_idtr.lim = sizeof tb_idt - 1; tb_idtr.base = (uint64_t)(uintptr_t)tb_idt;
    __asm__ volatile("lidt %0" : : "m"(tb_idtr));
    // контроллер прерываний: железо 0..15 → 32..47; таймер и клавиатура открыты
    tb_outb(0x20, 0x11); tb_outb(0xA0, 0x11); tb_outb(0x21, 0x20); tb_outb(0xA1, 0x28);
    tb_outb(0x21, 0x04); tb_outb(0xA1, 0x02); tb_outb(0x21, 0x01); tb_outb(0xA1, 0x01);
    tb_pic_mask = 0xFFFF & ~0x3u;
    tb_pic_apply();
    // таймер: 1000 раз в секунду
    tb_outb(0x43, 0x36); tb_outb(0x40, 1193182 / 1000 & 0xFF); tb_outb(0x40, (1193182 / 1000) >> 8);
    tb_paging_init();
    tb_slots[0].used = 1;
    tb_slots[0].on = 0;
    __asm__ volatile("sti");
    tb_smp_start();
    tb_kmain();
    exit(0);
}
// 32 бита → 64 бита: таблицы на 4 ГБ (страницы по 2 МБ), PAE, SSE, режим long, свой GDT
__asm__(".section .text.boot,\"ax\"\n.code32\n.global _start\n_start:\n"
        "  cli\n  movl %eax, tb_boot_magic\n  movl %ebx, tb_boot_mbi\n"
        "  movl $tb_boot_pdpt, %eax\n  orl $3, %eax\n  movl %eax, tb_boot_pml4\n"
        "  movl $tb_boot_pd, %eax\n  orl $3, %eax\n  movl $tb_boot_pdpt, %edi\n  movl $4, %ecx\n"
        "1:\n  movl %eax, (%edi)\n  addl $4096, %eax\n  addl $8, %edi\n  loop 1b\n"
        "  movl $tb_boot_pd, %edi\n  xorl %eax, %eax\n  movl $2048, %ecx\n"
        "2:\n  movl %eax, %edx\n  orl $0x83, %edx\n  movl %edx, (%edi)\n  movl $0, 4(%edi)\n  addl $0x200000, %eax\n  addl $8, %edi\n  loop 2b\n"
        "  movl $tb_boot_pml4, %eax\n  movl %eax, %cr3\n"
        "  movl %cr4, %eax\n  orl $0x620, %eax\n  movl %eax, %cr4\n"
        "  movl $0xC0000080, %ecx\n  rdmsr\n  orl $0x100, %eax\n  wrmsr\n"
        "  movl %cr0, %eax\n  andl $0xFFFFFFFB, %eax\n  orl $0x80000002, %eax\n  movl %eax, %cr0\n"
        "  lgdt tb_boot_gdtr\n  ljmp $0x08, $tb_start64\n"
        ".code64\ntb_start64:\n"
        "  movw $0x10, %ax\n  movw %ax, %ds\n  movw %ax, %es\n  movw %ax, %ss\n  movw %ax, %fs\n  movw %ax, %gs\n"
        "  movq $tb_stack + 1048576, %rsp\n  call tb_kstart\n3:\n  cli\n  hlt\n  jmp 3b\n"
        ".section .rodata.boot,\"a\"\n.balign 16\ntb_boot_gdt:\n  .quad 0, 0x00AF9A000000FFFF, 0x00CF92000000FFFF\n"
        "tb_boot_gdtr:\n  .word 23\n  .long tb_boot_gdt\n.text\n");
