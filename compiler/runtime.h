// функции библиотеки: внутри файла (static). Для --llvm файл .ll зовёт их через
// маленькие обёртки tb_x_* (их пишет компилятор в конце библиотеки), LTO встраивает всё обратно
#define TB_API static
#define TB_INL static inline
// то, что видно из .ll (shared, tb_show_N, tb_new_X, обёртки): с --llvm без static
#ifdef TB_LL
#define TB_LLV
#else
#define TB_LLV static
#endif
// Библиотека, которую Tachbit кладёт в начало каждой программы на C.
// Это обычный C. Компилятор на Tachbit встраивает этот текст через runtime_text.tb,
// который делает tools/embed_runtime.py. Поменял этот файл — запусти:
//     python3 tools/embed_runtime.py
#ifdef TB_BARE
// ---------- режим без ОС (tb bare): своя маленькая библиотека вместо libc ----------
// Программа сама себе ОС: загружается через multiboot (QEMU, GRUB), пишет на экран
// (текстовый режим VGA) и в последовательный порт COM1, память берёт из своего запаса.
// Есть прерывания, таймер, клавиатура, задачи (background) и страницы памяти; файлов нет.
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
// «переменные потока» лежат подряд в своём разделе: при смене задачи он копируется целиком
#define _Thread_local __attribute__((section(".tbtls")))
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

TB_INL void tb_outb(unsigned short port, unsigned char v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
TB_INL unsigned char tb_inb(unsigned short port) { unsigned char v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
TB_INL void tb_outw(unsigned short port, unsigned short v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
TB_INL unsigned short tb_inw(unsigned short port) { unsigned short v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
TB_INL void tb_outl(unsigned short port, unsigned int v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
TB_INL unsigned int tb_inl(unsigned short port) { unsigned int v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
// выключить прерывания на время короткого дела и вернуть, как было
TB_INL uint32_t tb_irq_save(void) { uint32_t f; __asm__ volatile("pushfl\npopl %0\ncli" : "=r"(f) : : "memory"); return f; }
TB_INL void tb_irq_restore(uint32_t f) { __asm__ volatile("pushl %0\npopfl" : : "r"(f) : "memory", "cc"); }

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
TB_API int fputs(const char *s, FILE *f) {
    uint32_t fl = tb_irq_save();
    unsigned char was = tb_vcolor;
    if (f == stderr) tb_vcolor = 0x0C;   // ошибки — красным
    while (*s) tb_putc(*s++);
    tb_vcolor = was;
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
TB_API int tb_cls(size_t n) { int k = 4; while (k < 31 && ((size_t)1 << k) < n) k++; return k; }
TB_API void *tb_malloc0(size_t n) {
    int k = tb_cls(n);
    if (tb_bins[k]) { void *p = tb_bins[k]; tb_bins[k] = *(void **)p; return p; }
    size_t need = ((size_t)1 << k) + 16;
    if (tb_heap_top + need > TB_HEAP) return 0;
    unsigned char *h = tb_heap + tb_heap_top;
    *(size_t *)h = (size_t)k;
    tb_heap_top += need;
    return h + 16;
}
TB_API void tb_free0(void *p) {
    if (p == 0) return;
    unsigned char *h = (unsigned char *)p - 16;
    int k = (int)*(size_t *)h;
    if (h + ((size_t)1 << k) + 16 == tb_heap + tb_heap_top) { tb_heap_top = (size_t)(h - tb_heap); return; }
    *(void **)p = tb_bins[k];
    tb_bins[k] = p;
}
// задачи и прерывания тоже берут память: на время выдачи прерывания выключены
TB_API void *malloc(size_t n) { uint32_t f = tb_irq_save(); void *p = tb_malloc0(n); tb_irq_restore(f); return p; }
TB_API void free(void *p) { uint32_t f = tb_irq_save(); tb_free0(p); tb_irq_restore(f); }
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
TB_API void *realloc(void *p, size_t n) { uint32_t f = tb_irq_save(); void *q = tb_realloc0(p, n); tb_irq_restore(f); return q; }
// одно ядро: «атомарное» сложение 64-битного числа — с выключенными на миг прерываниями
unsigned long long __atomic_fetch_add_8(volatile void *p, unsigned long long v, int order) {
    (void)order;
    uint32_t f = tb_irq_save();
    unsigned long long old = *(volatile unsigned long long *)p;
    *(volatile unsigned long long *)p = old + v;
    tb_irq_restore(f);
    return old;
}
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
TB_API double sqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
TB_API double fabs(double x) { return x < 0 ? -x : x; }
TB_API double strtod(const char *s, char **end) {
    double r = 0, sign = 1, scale = 1;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { r = r * 10 + (*s++ - '0'); scale *= 10; } }
    if (end) *end = (char *)s;
    return sign * r / scale;
}
// деление 64-битных чисел на 32-битном процессоре: этих функций без libgcc нет
unsigned long long __udivmoddi4(unsigned long long a, unsigned long long b, unsigned long long *rem) {
    // делитель влезает в 32 бита (почти всегда): две команды divl процессора вместо 64 шагов
    if ((b >> 32) == 0 && b != 0) {
        uint32_t d = (uint32_t)b, hi = (uint32_t)(a >> 32), lo = (uint32_t)a, qh = hi / d, rh = hi % d, ql, rl;
        __asm__("divl %4" : "=a"(ql), "=d"(rl) : "a"(lo), "d"(rh), "rm"(d));
        if (rem) *rem = rl;
        return ((unsigned long long)qh << 32) | ql;
    }
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
// файлов, ввода и команд нет
TB_API FILE *fopen(const char *p, const char *m) { (void)p; (void)m; return 0; }
TB_API int fclose(FILE *f) { (void)f; return 0; }
TB_API size_t fread(void *b, size_t s, size_t n, FILE *f) { (void)b; (void)s; (void)n; (void)f; return 0; }
TB_API int getchar(void) { return EOF; }
TB_API int system(const char *c) { (void)c; return -1; }
// ---------- ядро: прерывания, таймер, клавиатура, задачи, страницы памяти ----------
// on all cores здесь выполняется на одном ядре по очереди (как и было),
// а background — настоящая задача: таймер переключает задачи каждые 10 мс.
TB_API long sysconf(int n) { (void)n; return 1; }
TB_API int pthread_create(pthread_t *t, void *a, void *(*run)(void *), void *arg) { (void)t; (void)a; run(arg); return 0; }
TB_API int pthread_join(pthread_t t, void **r) { (void)t; (void)r; return 0; }

// адреса из файла сборки (link.ld): где код, где кончается нижняя память, где куча
extern char tb_code_start[], tb_code_end[], tb_tls_start[], tb_tls_end[], tb_heap_area[], tb_kernel_end[];
TB_API unsigned char tb_stack[1 << 20] __attribute__((aligned(4096), used));
void tb_kmain(void);

// авария ядра: красным на экран и в порт, потом стоп
__attribute__((noreturn)) static void tb_panic(const char *what, uint32_t a, uint32_t b) {
    __asm__ volatile("cli");
    char buf[160];
    snprintf(buf, sizeof buf, "\n*** ядро остановлено: %s (0x%x, 0x%x)\n", what, a, b);
    fputs(buf, stderr);
    exit(3);
}

// --- таблица сегментов: всё плоское, от 0 до 4 ГБ ---
TB_API uint64_t tb_gdt[5] = {0, 0x00CF9A000000FFFFULL, 0x00CF92000000FFFFULL, 0, 0};
// двойная ошибка (например, стек упёрся в сторожа и обработчику некуда класть) идёт
// через отдельную «задачу процессора» со своим стеком — так ядро успевает сказать, что случилось
TB_API uint32_t tb_tss_main[26], tb_tss_df[26];
TB_API unsigned char tb_df_stack[8192] __attribute__((aligned(16)));
TB_API uint64_t tb_tss_desc(uint32_t base) {
    return 103ull | ((uint64_t)(base & 0xFFFFFFu) << 16) | (0x89ull << 40) | ((uint64_t)(base >> 24) << 56);
}
TB_API uint64_t tb_idt[256];
TB_API struct __attribute__((packed)) { uint16_t lim; uint32_t base; } tb_gdtr, tb_idtr;

// 49 входов: 0..31 ошибки процессора, 32..47 железо (таймер, клавиатура ...), 48 — «уступить ход»
extern char tb_isr_base[];
__asm__(".text\n.balign 16\n.global tb_isr_base\ntb_isr_base:\n"
        ".set tb_v, 0\n.rept 49\n.balign 16\n"
        ".if (tb_v == 8) || (tb_v == 10) || (tb_v == 11) || (tb_v == 12) || (tb_v == 13) || (tb_v == 14) || (tb_v == 17) || (tb_v == 21)\n"
        ".else\npushl $0\n.endif\npushl $tb_v\njmp tb_isr_common\n.set tb_v, tb_v + 1\n.endr\n"
        "tb_isr_common:\ncld\npusha\npushl %esp\ncall tb_isr\nmovl %eax, %esp\npopa\naddl $8, %esp\niret\n");

// --- память задач: у каждой свои «переменные потока» (temp, кэш длины) и регистры дробей ---
#define TB_TLS_MAX 512
#define TB_NTASK 32
#define TB_TSTACK 65536
typedef struct { pthread_t th; volatile int done; void *res; } tb_task;
typedef struct {
    uint32_t esp; int used; volatile int done;
    void *(*run)(void *); void *arg; tb_task *t;
    unsigned char tls[TB_TLS_MAX]; unsigned char fpu[108];
} tb_kslot;
TB_API tb_kslot tb_slots[TB_NTASK];          // 0 — main
TB_API unsigned char tb_tstacks[TB_NTASK][TB_TSTACK] __attribute__((aligned(4096)));
TB_API int tb_cur = 0, tb_nlive = 1, tb_slice = 10, tb_in_irq = 0;
TB_API unsigned char tb_tls_zero[TB_TLS_MAX], tb_fpu_init[108];
TB_API unsigned char tb_irq_tls[TB_TLS_MAX];
TB_API volatile uint64_t tb_ms = 0;
TB_API void (*tb_irq_fn[16])(void);
TB_API uint16_t tb_pic_mask = 0xFFFF;
TB_API uint32_t tb_tls_size(void) { return (uint32_t)(tb_tls_end - tb_tls_start); }

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

// переключение задач: сохранить «переменные потока» и дроби текущей, взять следующей
TB_API uint32_t tb_schedule(uint32_t esp) {
    if (tb_nlive <= 1 && !tb_slots[tb_cur].done) return esp;
    int n = tb_cur;
    for (int k = 1; k <= TB_NTASK; k++) {
        int j = (tb_cur + k) % TB_NTASK;
        if (tb_slots[j].used && !tb_slots[j].done) { n = j; break; }
    }
    tb_kslot *c = &tb_slots[tb_cur];
    if (n == tb_cur) return esp;
    c->esp = esp;
    memcpy(c->tls, tb_tls_start, tb_tls_size());
    __asm__ volatile("fnsave %0" : "=m"(c->fpu));
    if (c->done) { c->used = 0; tb_nlive--; }
    tb_cur = n;
    memcpy(tb_tls_start, tb_slots[n].tls, tb_tls_size());
    __asm__ volatile("frstor %0" : : "m"(tb_slots[n].fpu));
    tb_slice = 10;
    return tb_slots[n].esp;
}
TB_API void tb_task_entry(void) {
    tb_kslot *s = &tb_slots[tb_cur];
    s->run(s->arg);
    __asm__ volatile("cli");
    s->t->done = 1;
    s->done = 1;
    for (;;) __asm__ volatile("int $48");
}
TB_API void tb_task_start(tb_task *t, void *(*run)(void *), void *arg) {
    uint32_t f = tb_irq_save();
    int i = 1;
    while (i < TB_NTASK && tb_slots[i].used) i++;
    if (i == TB_NTASK) tb_panic("больше 31 задачи background сразу", 0, 0);
    tb_kslot *s = &tb_slots[i];
    s->used = 1; s->done = 0; s->run = run; s->arg = arg; s->t = t;
    t->done = 0; t->th = i;
    memcpy(s->tls, tb_tls_zero, TB_TLS_MAX);
    memcpy(s->fpu, tb_fpu_init, 108);
    // стек задачи выглядит так, будто её прервали прямо перед tb_task_entry
    uint32_t *sp = (uint32_t *)(tb_tstacks[i] + TB_TSTACK);
    *--sp = 0x202; *--sp = 0x08; *--sp = (uint32_t)tb_task_entry; *--sp = 0; *--sp = 0;
    for (int k = 0; k < 8; k++) *--sp = 0;
    s->esp = (uint32_t)sp;
    tb_nlive++;
    tb_irq_restore(f);
}
// a.wait: пока задача работает, отдаём ход другим
TB_API void *tb_wait(tb_task *t) {
    if (tb_in_irq && !t->done) tb_panic("wait внутри прерывания нельзя", 0, 0);
    while (!t->done) tb_yield();
    return t->res;
}

// --- время ---
TB_API long long tb_uptime_ms(void) { uint32_t f = tb_irq_save(); uint64_t v = tb_ms; tb_irq_restore(f); return (long long)v; }
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
TB_API char tb_keys[256];
TB_API volatile unsigned char tb_khead = 0, tb_ktail = 0;
TB_API int tb_kshift = 0;
TB_API char tb_ktext[256][2];
TB_API void tb_key_irq(void) {
    unsigned char sc = tb_inb(0x60);
    if (sc == 0x2A || sc == 0x36) { tb_kshift = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { tb_kshift = 0; return; }
    if (sc >= 58) return;
    char ch = tb_kmap[tb_kshift][sc];
    if (ch == 0 || (unsigned char)(tb_khead + 1) == tb_ktail) return;
    tb_keys[tb_khead] = ch;
    tb_khead = (unsigned char)(tb_khead + 1);
}
TB_API const char *tb_read_key(void) {
    uint32_t f = tb_irq_save();
    const char *r = "";
    if (tb_ktail != tb_khead) { r = tb_ktext[(unsigned char)tb_keys[tb_ktail]]; tb_ktail = (unsigned char)(tb_ktail + 1); }
    tb_irq_restore(f);
    return r;
}

// --- страницы памяти: адрес 0 и код защищены, у стеков — пустая страница-сторож ---
#define TB_LOW_TABLES 8
TB_API uint32_t tb_pd[1024] __attribute__((aligned(4096)));
TB_API uint32_t tb_pt[TB_LOW_TABLES][1024] __attribute__((aligned(4096)));
TB_API uint32_t tb_ram_end = 0, tb_pg_next = 0, tb_pg_free = 0;
TB_API void tb_unmap(uint32_t a) { tb_pt[a >> 22][(a >> 12) & 1023] = 0; }
TB_API int tb_is_guard(uint32_t a) {
    if (a >= (uint32_t)tb_stack && a < (uint32_t)tb_stack + 4096) return 1;
    for (int i = 1; i < TB_NTASK; i++)
        if (a >= (uint32_t)tb_tstacks[i] && a < (uint32_t)tb_tstacks[i] + 4096) return 1;
    return 0;
}
TB_API void tb_paging_init(void) {
    uint32_t low = (uint32_t)tb_heap_area;
    if (low > TB_LOW_TABLES * 0x400000u) tb_panic("ядро слишком большое для нижних страниц", low, 0);
    if (tb_ram_end < (uint32_t)tb_kernel_end + 0x400000u) tb_panic("мало памяти: запусти qemu с -m 256", tb_ram_end, (uint32_t)tb_kernel_end);
    for (uint32_t a = 0; a < low; a += 4096) {
        uint32_t flags = 3;                                   // есть, можно писать
        if (a >= (uint32_t)tb_code_start && a < (uint32_t)tb_code_end) flags = 1;   // код: только читать
        tb_pt[a >> 22][(a >> 12) & 1023] = a | flags;
    }
    tb_unmap(0);                                              // адрес 0: «пустая ссылка»
    tb_unmap((uint32_t)tb_stack);                             // сторож под стеком main
    for (int i = 1; i < TB_NTASK; i++) tb_unmap((uint32_t)tb_tstacks[i]);
    for (uint32_t t = 0; t < low >> 22; t++) tb_pd[t] = (uint32_t)tb_pt[t] | 3;
    // выше — большие страницы по 4 МБ: куча и свободная память (меньше промахов TLB, быстрее)
    for (uint32_t a = low; a < tb_ram_end && a >= low; a += 0x400000u) tb_pd[a >> 22] = a | 0x83;
    __asm__ volatile("movl %%cr4, %%eax\norl $0x10, %%eax\nmovl %%eax, %%cr4\n"
                     "movl %0, %%cr3\n"
                     "movl %%cr0, %%eax\norl $0x80010000, %%eax\nmovl %%eax, %%cr0\n" : : "r"(tb_pd) : "eax", "memory");
    tb_pg_next = (uint32_t)tb_kernel_end;
}
TB_API long long tb_page_new(void) {
    uint32_t f = tb_irq_save(), p = 0;
    if (tb_pg_free) { p = tb_pg_free; tb_pg_free = *(uint32_t *)p; }
    else if (tb_pg_next + 4096 <= tb_ram_end) { p = tb_pg_next; tb_pg_next += 4096; }
    tb_irq_restore(f);
    if (p == 0) tb_panic("свободные страницы памяти кончились", tb_ram_end, 0);
    memset((void *)p, 0, 4096);
    return p;
}
TB_API void tb_page_free(long long a) {
    uint32_t p = (uint32_t)a;
    if ((p & 4095) || p < (uint32_t)tb_kernel_end || p >= tb_pg_next) tb_panic("page_free: это не страница из page_new", p, 0);
    uint32_t f = tb_irq_save();
    *(uint32_t *)p = tb_pg_free;
    tb_pg_free = p;
    tb_irq_restore(f);
}

// --- ошибки процессора ---
TB_API const char *tb_fault_name[32] = {"деление на ноль", "отладка", "NMI", "точка останова", "переполнение",
    "выход за границы", "неизвестная команда", "нет FPU", "двойная ошибка", "", "плохой TSS", "нет сегмента",
    "ошибка стека", "нарушение защиты", "ошибка страницы", "", "ошибка дробей", "выравнивание", "проверка железа", "ошибка SIMD"};
TB_API void tb_fault(uint32_t v, uint32_t *f) {
    uint32_t eip = f[10];
    if (v == 14) {
        uint32_t a;
        __asm__ volatile("movl %%cr2, %0" : "=r"(a));
        if (a < 4096) tb_panic("обращение по пустому адресу (адрес 0)", a, eip);
        if (tb_is_guard(a)) tb_panic("стек переполнен: слишком глубокая рекурсия", a, eip);
        if ((f[9] & 2) && a >= (uint32_t)tb_code_start && a < (uint32_t)tb_code_end) tb_panic("запись в код программы", a, eip);
        tb_panic("обращение к памяти, которой нет", a, eip);
    }
    tb_panic(v < 20 && tb_fault_name[v][0] ? tb_fault_name[v] : "ошибка процессора", v, eip);
}

__attribute__((noreturn)) static void tb_double_fault(void) {
    uint32_t a;
    __asm__ volatile("movl %%cr2, %0" : "=r"(a));
    if (tb_is_guard(a)) tb_panic("стек переполнен: слишком глубокая рекурсия", a, 0);
    tb_panic("двойная ошибка процессора", a, 0);
}

// все прерывания приходят сюда; ответ — стек, на который вернуться (так переключаются задачи)
__attribute__((used)) static uint32_t tb_isr(uint32_t *f) {
    uint32_t v = f[8];
    if (v < 32) tb_fault(v, f);
    if (v == 48) return tb_schedule((uint32_t)f);
    int irq = (int)v - 32;
    if (irq == 0) tb_ms++;
    if (irq == 1) tb_key_irq();
    if (tb_irq_fn[irq]) {
        // своя функция прерывания: со своими «переменными потока», дроби задачи не трогает
        unsigned char save_tls[TB_TLS_MAX], save_fpu[108];
        uint32_t n = tb_tls_size();
        memcpy(save_tls, tb_tls_start, n);
        memcpy(tb_tls_start, tb_irq_tls, n);
        __asm__ volatile("fnsave %0" : "=m"(save_fpu));
        tb_in_irq = 1;
        tb_irq_fn[irq]();
        tb_in_irq = 0;
        __asm__ volatile("frstor %0" : : "m"(save_fpu));
        memcpy(tb_irq_tls, tb_tls_start, n);
        memcpy(tb_tls_start, save_tls, n);
    }
    if (irq >= 8) tb_outb(0xA0, 0x20);
    tb_outb(0x20, 0x20);
    if (irq == 0 && tb_nlive > 1 && --tb_slice <= 0) return tb_schedule((uint32_t)f);
    return (uint32_t)f;
}

// загрузка: заголовок multiboot (флаг 2: «скажи, сколько памяти»), свой стек, потом tb_kstart
__attribute__((section(".multiboot"), used, aligned(4)))
TB_API const unsigned int tb_multiboot[3] = {0x1BADB002u, 2u, (unsigned int)-(0x1BADB002 + 2)};
__attribute__((used)) static void tb_kstart(uint32_t magic, uint32_t *mbi) {
    __asm__ volatile("fninit");
    __asm__ volatile("fnsave %0\nfninit" : "=m"(tb_fpu_init));
    // COM1: 115200 бод, 8 бит
    tb_outb(0x3F8 + 1, 0x00); tb_outb(0x3F8 + 3, 0x80); tb_outb(0x3F8 + 0, 0x01);
    tb_outb(0x3F8 + 1, 0x00); tb_outb(0x3F8 + 3, 0x03); tb_outb(0x3F8 + 2, 0xC7);
    tb_vga_font();
    for (int i = 0; i < 80 * 25; i++) tb_vga[i] = (unsigned short)(tb_vcolor << 8 | ' ');
    tb_outb(0x3D4, 0x0A); tb_outb(0x3D5, 0x20);  // мигающий курсор не нужен
    tb_ram_end = 128u << 20;
    if (magic == 0x2BADB002u && (mbi[0] & 1)) tb_ram_end = (1u << 20) + mbi[2] * 1024u;
    if (tb_ram_end > 0xFFC00000u || tb_ram_end < (1u << 20)) tb_ram_end = 0xFFC00000u;
    if (tb_tls_size() > TB_TLS_MAX) tb_panic("переменных потока больше, чем место для них", tb_tls_size(), 0);
    memcpy(tb_tls_zero, tb_tls_start, tb_tls_size());
    memcpy(tb_irq_tls, tb_tls_start, tb_tls_size());
    for (int i = 0; i < 256; i++) { tb_ktext[i][0] = (char)i; tb_ktext[i][1] = 0; }
    // сегменты
    tb_gdt[3] = tb_tss_desc((uint32_t)tb_tss_main);
    tb_gdt[4] = tb_tss_desc((uint32_t)tb_tss_df);
    tb_tss_df[7] = (uint32_t)tb_pd;                        // cr3
    tb_tss_df[8] = (uint32_t)tb_double_fault;              // eip
    tb_tss_df[9] = 0x2;                                    // eflags
    tb_tss_df[14] = (uint32_t)(tb_df_stack + sizeof tb_df_stack);   // esp
    tb_tss_df[19] = 0x08;                                  // cs
    tb_tss_df[18] = tb_tss_df[20] = tb_tss_df[21] = tb_tss_df[22] = tb_tss_df[23] = 0x10;   // es ss ds fs gs
    tb_gdtr.lim = sizeof tb_gdt - 1; tb_gdtr.base = (uint32_t)tb_gdt;
    __asm__ volatile("lgdt %0\nljmp $0x08, $1f\n1:\nmovw $0x10, %%ax\nmovw %%ax, %%ds\nmovw %%ax, %%es\n"
                     "movw %%ax, %%fs\nmovw %%ax, %%gs\nmovw %%ax, %%ss\n" : : "m"(tb_gdtr) : "eax", "memory");
    // прерывания
    for (int v = 0; v < 49; v++) {
        uint32_t a = (uint32_t)tb_isr_base + 16u * (uint32_t)v;
        tb_idt[v] = (a & 0xFFFFu) | (0x08ull << 16) | (0x8Eull << 40) | ((uint64_t)(a >> 16) << 48);
    }
    tb_idt[8] = (0x20ull << 16) | (0x85ull << 40);         // двойная ошибка → своя задача
    tb_idtr.lim = sizeof tb_idt - 1; tb_idtr.base = (uint32_t)tb_idt;
    __asm__ volatile("lidt %0" : : "m"(tb_idtr));
    __asm__ volatile("ltr %w0" : : "r"(0x18));
    // контроллер прерываний: железо 0..15 → 32..47; таймер и клавиатура открыты
    tb_outb(0x20, 0x11); tb_outb(0xA0, 0x11); tb_outb(0x21, 0x20); tb_outb(0xA1, 0x28);
    tb_outb(0x21, 0x04); tb_outb(0xA1, 0x02); tb_outb(0x21, 0x01); tb_outb(0xA1, 0x01);
    tb_pic_mask = 0xFFFF & ~0x3u;
    tb_pic_apply();
    // таймер: 1000 раз в секунду
    tb_outb(0x43, 0x36); tb_outb(0x40, 1193182 / 1000 & 0xFF); tb_outb(0x40, (1193182 / 1000) >> 8);
    tb_paging_init();
    tb_slots[0].used = 1;
    __asm__ volatile("sti");
    tb_kmain();
    exit(0);
}
__asm__(".text\n.global _start\n_start:\n  movl $tb_stack + 1048576, %esp\n  pushl %ebx\n  pushl %eax\n  call tb_kstart\n1: cli\n  hlt\n  jmp 1b\n");
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <sched.h>
#include <dirent.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
// память с выравниванием (для сборщика мусора и др.): на Windows нет posix_memalign
TB_INL void *tb_aligned_alloc(size_t align, size_t n) {
#ifdef _WIN32
    return _aligned_malloc(n, align);
#else
    void *p = 0;
    return posix_memalign(&p, align, n) == 0 ? p : 0;
#endif
}
TB_INL void tb_aligned_free(void *p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}
#ifdef _WIN32
// ---------- Windows (MinGW-w64: gcc или clang) ----------
// Пути, слова запуска и переменные окружения в Tachbit — UTF-8. Windows для
// «узких» функций (fopen, mkdir, getenv...) берёт кодовую страницу ANSI, и русские
// имена ломаются. Поэтому здесь свои обёртки через «широкие» функции (_wfopen...),
// а имена POSIX ниже подменяются на них — код библиотеки остаётся одним на все системы.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <malloc.h>
TB_API wchar_t *tb_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, 0, 0);
    wchar_t *w = malloc((size_t)(n > 0 ? n : 1) * sizeof(wchar_t));
    if (!w) return 0;
    if (n <= 0) w[0] = 0; else MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
TB_API char *tb_narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
    char *s = malloc((size_t)(n > 0 ? n : 1));
    if (!s) return 0;
    if (n <= 0) s[0] = 0; else WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, 0, 0);
    return s;
}
TB_API FILE *tb_win_fopen(const char *path, const char *mode) {
    wchar_t *p = tb_wide(path), *m = tb_wide(mode);
    FILE *f = p && m ? _wfopen(p, m) : 0;
    free(p); free(m);
    return f;
}
TB_API int tb_win_stat(const char *path, struct stat *st) {
    wchar_t *p = tb_wide(path);
    if (!p) return -1;
    // «папка/» с косой чертой в конце _wstat не находит
    size_t n = wcslen(p);
    while (n > 1 && (p[n - 1] == L'/' || p[n - 1] == L'\\') && p[n - 2] != L':') p[--n] = 0;
    struct _stat64 s;
    int r = _wstat64(p, &s);
    free(p);
    if (r != 0) return r;
    memset(st, 0, sizeof *st);
    st->st_mode = s.st_mode;
    st->st_size = (_off_t)s.st_size;
    st->st_mtime = (time_t)s.st_mtime;
    return 0;
}
TB_API int tb_win_mkdir(const char *path) { wchar_t *p = tb_wide(path); int r = p ? _wmkdir(p) : -1; free(p); return r; }
TB_API int tb_win_rmdir(const char *path) { wchar_t *p = tb_wide(path); int r = p ? _wrmdir(p) : -1; free(p); return r; }
TB_API int tb_win_unlink(const char *path) { wchar_t *p = tb_wide(path); int r = p ? _wunlink(p) : -1; free(p); return r; }
// как в POSIX: файл на месте назначения заменяется (обычный rename на Windows так не умеет)
TB_API int tb_win_rename(const char *from, const char *to) {
    wchar_t *a = tb_wide(from), *b = tb_wide(to);
    int ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    DWORD e = ok ? 0 : GetLastError();
    free(a); free(b);
    if (ok) return 0;
    errno = e == ERROR_NOT_SAME_DEVICE ? EXDEV : e == ERROR_ACCESS_DENIED ? EACCES : ENOENT;
    return -1;
}
// getenv в UTF-8; ответ живёт до конца программы (их мало и они короткие).
// HOME на Windows обычно нет: тогда это USERPROFILE (C:\Users\имя)
TB_API char *tb_win_getenv(const char *name) {
    wchar_t *n = tb_wide(name);
    const wchar_t *v = n ? _wgetenv(n) : 0;
    if (!v && strcmp(name, "HOME") == 0) v = _wgetenv(L"USERPROFILE");
    free(n);
    return v ? tb_narrow(v) : 0;
}
// cmd.exe /c убирает первую и последнюю кавычку, если кавычек больше двух:
// поэтому команда целиком берётся ещё в одни кавычки
TB_API wchar_t *tb_win_cmd(const char *command) {
    size_t n = strlen(command);
    char *q = malloc(n + 3);
    if (!q) return 0;
    q[0] = '"'; memcpy(q + 1, command, n); q[n + 1] = '"'; q[n + 2] = 0;
    wchar_t *w = tb_wide(q);
    free(q);
    return w;
}
TB_API int tb_win_system(const char *command) {
    wchar_t *c = tb_win_cmd(command);
    int r = c ? _wsystem(c) : -1;
    free(c);
    return r;
}
TB_API FILE *tb_win_popen(const char *command, const char *mode) {
    wchar_t *c = tb_win_cmd(command), *m = tb_wide(mode);
    FILE *f = c && m ? _wpopen(c, m) : 0;
    free(c); free(m);
    return f;
}
// папка: _wopendir / _wreaddir, имена — в UTF-8
struct tb_win_dirent { char d_name[MAX_PATH * 4]; };
typedef struct { _WDIR *d; struct tb_win_dirent e; } tb_win_dir;
TB_API tb_win_dir *tb_win_opendir(const char *path) {
    wchar_t *p = tb_wide(path);
    _WDIR *d = p ? _wopendir(p) : 0;
    free(p);
    if (!d) return 0;
    tb_win_dir *r = calloc(1, sizeof *r);
    if (!r) { _wclosedir(d); return 0; }
    r->d = d;
    return r;
}
TB_API struct tb_win_dirent *tb_win_readdir(tb_win_dir *d) {
    struct _wdirent *e = _wreaddir(d->d);
    if (!e) return 0;
    if (WideCharToMultiByte(CP_UTF8, 0, e->d_name, -1, d->e.d_name, sizeof d->e.d_name, 0, 0) <= 0) d->e.d_name[0] = 0;
    return &d->e;
}
TB_API int tb_win_closedir(tb_win_dir *d) { _wclosedir(d->d); free(d); return 0; }
// цвет в консоли — только если удалось включить ESC-последовательности (Windows 10+)
TB_API int tb_win_vt = 0;
TB_API int tb_win_isatty(int fd) { return tb_win_vt && _isatty(fd); }
// число ядер
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif
TB_API long tb_win_sysconf(int what) {
    (void)what;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (long)si.dwNumberOfProcessors;
}
#define fopen(p, m) tb_win_fopen(p, m)
#define stat(p, s) tb_win_stat(p, s)
#define mkdir(p, mode) tb_win_mkdir(p)
#define rmdir(p) tb_win_rmdir(p)
#define unlink(p) tb_win_unlink(p)
#define rename(a, b) tb_win_rename(a, b)
#define getenv(n) tb_win_getenv(n)
#undef popen
#undef pclose
#define popen(c, m) tb_win_popen(c, m)
#define pclose(f) _pclose(f)
#define DIR tb_win_dir
#define dirent tb_win_dirent
#define opendir(p) tb_win_opendir(p)
#define readdir(d) tb_win_readdir(d)
#define closedir(d) tb_win_closedir(d)
#define isatty(fd) tb_win_isatty(fd)
#define sysconf(x) tb_win_sysconf(x)
#define localtime_r(t, r) (localtime_s((r), (t)) == 0 ? (r) : 0)
#endif
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

TB_COLD TB_API void tb_crash(int line, const char *msg) {
    fflush(stdout);
    // в терминале — красным (если не задан NO_COLOR)
    int color = isatty(2) && getenv("NO_COLOR") == 0;
    if (color) fputs("\033[31m", stderr);
    if (line > 0) fprintf(stderr, "строка %d: %s", line, msg);
    else fprintf(stderr, "%s", msg);
    fputs(color ? "\033[0m\n" : "\n", stderr);
    exit(1);
}
#ifndef TB_BARE
// ---------- падение по памяти: понятное сообщение вместо «Segmentation fault» ----------
// Чаще всего это стек: функция зовёт себя слишком глубоко. Обработчик работает
// на своём маленьком стеке (sigaltstack), потому что основной уже кончился.
#ifndef _WIN32
#include <signal.h>
#include <sys/resource.h>
#else
// На Windows нет сигналов и sigaltstack: переполнение стека ловит обработчик
// исключений (tb_guard_init ниже), а место для него в конце стека даёт
// SetThreadStackGuarantee. Этот sigaltstack только помнит память потока,
// чтобы tb_tramp_run мог её отдать, и просит у Windows запас стека.
typedef struct { void *ss_sp; size_t ss_size; int ss_flags; } stack_t;
#define SS_DISABLE 4
TB_API _Thread_local void *tb_win_alt;
TB_API int sigaltstack(const stack_t *ss, stack_t *old) {
    if (old) { old->ss_sp = tb_win_alt; old->ss_size = 0; old->ss_flags = 0; }
    if (ss) {
        tb_win_alt = (ss->ss_flags & SS_DISABLE) ? 0 : ss->ss_sp;
        if (!(ss->ss_flags & SS_DISABLE)) { ULONG g = 65536; SetThreadStackGuarantee(&g); }
    }
    return 0;
}
#endif
// у каждого потока своё: где начинается его стек и сколько в нём места
TB_API _Thread_local char *tb_main_sp;
TB_API _Thread_local long long tb_stack_lim;
#define TB_THREAD_STACK (8LL << 20)
#ifndef _WIN32
TB_API void tb_on_segv(int sig, siginfo_t *si, void *u) {
    (void)sig; (void)u;
    char *a = (char *)si->si_addr;
    long long lim = tb_stack_lim;
    if (lim == 0) {
        lim = 8LL << 20;
        struct rlimit rl;
        if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) lim = (long long)rl.rlim_cur;
    }
    const char *m;
    if (tb_main_sp && a <= tb_main_sp + 65536 && a >= tb_main_sp - lim - (1 << 20))
        m = "стек переполнен: функция зовёт себя слишком глубоко (рекурсия без конца?)\n";
    else
        m = "программа упала: ошибка памяти. Это ошибка Tachbit, а не твоя: пришли, пожалуйста, эту программу\n";
    if (write(2, m, strlen(m)) < 0) _exit(1);
    _exit(1);
}
#endif
// свой маленький стек для сообщения: у главного потока — постоянный, у остальных — из malloc
TB_API void tb_guard_thread(char *here, long long lim) {
    stack_t ss;
    ss.ss_sp = malloc(65536); ss.ss_size = 65536; ss.ss_flags = 0;
    if (ss.ss_sp) sigaltstack(&ss, 0);
    tb_main_sp = here;
    tb_stack_lim = lim;
}
// поток background или ядра: сначала ставим защиту стека, потом сама работа
// сколько потоков background / ядер работает сейчас: уборка памяти ждёт, пока их нет
TB_API int tb_gthreads = 0;
// короткое ожидание без сна (подсказка процессору)
#if defined(__x86_64__) || defined(__i386__)
#define __builtin_ia32_pause_or_yield() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define __builtin_ia32_pause_or_yield() __asm__ volatile("yield")
#else
#define __builtin_ia32_pause_or_yield() ((void)0)
#endif
typedef struct { void *(*run)(void *); void *arg; } tb_tramp;
TB_API void *tb_tramp_run(void *p) {
    tb_tramp t = *(tb_tramp *)p;
    free(p);
    char here;
    tb_guard_thread(&here, TB_THREAD_STACK);
    void *r = t.run(t.arg);
    __atomic_sub_fetch(&tb_gthreads, 1, __ATOMIC_RELEASE);
    stack_t ss, old;
    ss.ss_sp = 0; ss.ss_size = 0; ss.ss_flags = SS_DISABLE;
    if (sigaltstack(&ss, &old) == 0 && old.ss_sp) free(old.ss_sp);
    return r;
}
// поток с 8 МБ стека (на Mac по умолчанию всего 512 КБ) и с понятным сообщением при переполнении
TB_API int tb_thread_create(pthread_t *th, void *(*run)(void *), void *arg) {
    tb_tramp *t = malloc(sizeof(tb_tramp));
    if (!t) return -1;
    t->run = run; t->arg = arg;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, (size_t)TB_THREAD_STACK);
    __atomic_add_fetch(&tb_gthreads, 1, __ATOMIC_ACQ_REL);
    int r = pthread_create(th, &at, tb_tramp_run, t);
    pthread_attr_destroy(&at);
    if (r != 0) { free(t); __atomic_sub_fetch(&tb_gthreads, 1, __ATOMIC_RELEASE); }
    return r;
}
#ifdef _WIN32
TB_API void tb_win_fatal(const char *m) {
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), m, (DWORD)strlen(m), &w, 0);
    TerminateProcess(GetCurrentProcess(), 1);   // как _exit(1): без уборки, она может упасть ещё раз
}
#define TB_WIN_STACK_MSG "стек переполнен: функция зовёт себя слишком глубоко (рекурсия без конца?)\n"
// переполнение стека — сразу (это никогда не «нормальное» исключение)
TB_API LONG CALLBACK tb_win_overflow(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) tb_win_fatal(TB_WIN_STACK_MSG);
    return EXCEPTION_CONTINUE_SEARCH;
}
// остальные падения — только если их никто не поймал
TB_API LONG WINAPI tb_win_crashed(EXCEPTION_POINTERS *e) {
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_STACK_OVERFLOW) tb_win_fatal(TB_WIN_STACK_MSG);
    if (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_IN_PAGE_ERROR || c == EXCEPTION_GUARD_PAGE) {
        char *a = e->ExceptionRecord->NumberParameters >= 2 ? (char *)e->ExceptionRecord->ExceptionInformation[1] : 0;
        long long lim = tb_stack_lim ? tb_stack_lim : 8LL << 20;
        if (tb_main_sp && a <= tb_main_sp + 65536 && a >= tb_main_sp - lim - (1 << 20)) tb_win_fatal(TB_WIN_STACK_MSG);
        tb_win_fatal("программа упала: ошибка памяти. Это ошибка Tachbit, а не твоя: пришли, пожалуйста, эту программу\n");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
// начало программы на Windows: русский текст в консоли, цвета, слова запуска в UTF-8,
// понятное сообщение при переполнении стека
TB_API void tb_win_init(int *argc, char ***argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // \n остаётся \n (как на Linux и Mac): одинаковый вывод в файл и в трубу
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    DWORD mode;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    if (GetConsoleMode(out, &mode)) SetConsoleMode(out, mode | 0x0004);   // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    if (GetConsoleMode(err, &mode) && SetConsoleMode(err, mode | 0x0004)) tb_win_vt = 1;
    int n = 0;
    wchar_t **wa = CommandLineToArgvW(GetCommandLineW(), &n);
    if (wa && n > 0) {
        char **a = malloc(sizeof(char *) * (size_t)(n + 1));
        if (a) {
            for (int i = 0; i < n; i++) a[i] = tb_narrow(wa[i]);
            a[n] = 0;
            *argc = n; *argv = a;
        }
        LocalFree(wa);
    }
    AddVectoredExceptionHandler(1, tb_win_overflow);
    SetUnhandledExceptionFilter(tb_win_crashed);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ULONG g = 65536;
    SetThreadStackGuarantee(&g);
    char here;
    tb_main_sp = &here;
}
// main() зовёт tb_guard_init(): на Windows ей нужны ещё argc и argv из main
#define tb_guard_init() tb_win_init(&argc, &argv)
#else
TB_API void tb_guard_init(void) {
    static char alt[65536];
    stack_t ss;
    ss.ss_sp = alt; ss.ss_size = sizeof alt; ss.ss_flags = 0;
    sigaltstack(&ss, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = tb_on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    char here;
    tb_main_sp = &here;
}
#endif
#endif
// ---------- temp: временная память ----------
// Внутри блока temp память берётся из больших кусков подряд (быстро, без malloc),
// а в конце блока все куски отдаются разом. У каждого ядра свои куски.
typedef struct { char *mem; long long size, used; } tb_chunk;
typedef struct { int n; long long used; } tb_mark;
TB_API _Thread_local tb_chunk *tb_chunks = 0;
TB_API _Thread_local int tb_nchunks = 0, tb_capchunks = 0, tb_temp_depth = 0;
TB_API int tb_in_temp(const void *p) {
    for (int i = tb_nchunks - 1; i >= 0; i--) {
        if ((const char *)p >= tb_chunks[i].mem && (const char *)p < tb_chunks[i].mem + tb_chunks[i].size) return 1;
    }
    return 0;
}
TB_API void *tb_temp_alloc(long long n) {
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
TB_API _Thread_local tb_mark *tb_marks = 0;
TB_API _Thread_local int tb_capmarks = 0;
TB_INL tb_mark tb_temp_begin(void) {
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
TB_API int tb_in_cur_temp(const void *p) {
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
TB_API _Thread_local const char *tb_lc_p[2];
TB_API _Thread_local long long tb_lc_n[2];
TB_API _Thread_local int tb_lc_next;
TB_INL void tb_lc_clear(void) { tb_lc_p[0] = 0; tb_lc_p[1] = 0; }
TB_INL void tb_temp_end(tb_mark m) {
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
// --llvm: то же через указатель (из файла .ll так проще, чем отдавать struct по значению)
TB_INL void tb_temp_begin_p(tb_mark *m) { *m = tb_temp_begin(); }
TB_INL void tb_temp_end_p(tb_mark *m) { tb_temp_end(*m); }
// память функции: своя область, только если функцию позвали не изнутри другой области
TB_INL int tb_fa_begin(tb_mark *m) { if (tb_temp_depth != 0) return 0; *m = tb_temp_begin(); return 1; }
TB_INL void tb_fa_end(int fa, tb_mark *m) { if (fa) tb_temp_end(*m); }
// ---------- уборка памяти: сборщик мусора ----------
// Шаги циклов, функции и temp уже отдают память сами (своя временная память).
// А то, что живёт дольше (obj, списки, dict, тексты), раньше жило до конца программы.
// Теперь иногда (когда набралось много новой памяти) программа делает обход:
//   1) смотрит, что видно из переменных: стек, общие (shared) переменные, временная память;
//   2) от них идёт по всему, на что они указывают, и помечает это «нужно»;
//   3) всё не помеченное — мусор, его место отдаётся под новые значения.
// Обход идёт, только когда работает один поток: пока идут background или on all cores, ждём.
// TB_GC=0 — выключить уборку (для проверки), TB_GC_STRESS=N — обход на каждом N-м выделении,
// TB_GC_STATS=1 — печатать каждый обход.
#ifdef TB_BARE
TB_API void *tb_gc_alloc(long long n) {
    void *p = malloc(n > 0 ? n : 1);
    if (p == 0) tb_crash(0, "не хватило памяти");
    return p;
}
TB_API void tb_gc_free(void *p) { free(p); }
#define tb_gc_alloc_np tb_gc_alloc
TB_API void *tb_gc_realloc(void *p, long long old_n, long long new_n) {
    (void)old_n;
    p = realloc(p, new_n > 0 ? new_n : 1);
    if (p == 0) tb_crash(0, "не хватило памяти");
    return p;
}
#define tb_gc_root(p, n) ((void)(p), (void)(n))
#else
#include <setjmp.h>
#define TB_GSH 16
#define TB_GPG (1LL << TB_GSH)
#define TB_GNC 37
#define TB_GBIG 32768
// размеры мелких блоков: страница 64 КБ делится на блоки одного размера (шаг 8 байт у мелких — плотно, как подряд)
TB_API const long long tb_gsz[TB_GNC] = {16, 24, 32, 40, 48, 56, 64, 72, 80, 96, 112, 128, 160, 192, 224, 256,
    320, 384, 448, 512, 640, 768, 1024, 1280, 1536, 2048, 2560, 3072, 4096, 5120, 6144, 8192, 10240, 12288,
    16384, 21840, 32768};
typedef struct tb_gpage {
    char *start;
    long long bsize, nblocks;      // мелкие: размер блока и сколько их
    long long nbytes, cap;         // большой блок: сколько занято и сколько взято
    unsigned long long inv;        // 2^32 / bsize (вверх): номер блока умножением, без деления
    int large, cls, in_avail, np;  // np: внутри нет адресов (числа, тексты) — обход туда не смотрит
    unsigned char *fl;             // по байту на блок: 1 — занят, 2 — нужен (найден при обходе)
    unsigned char lfl;             // то же для большого блока
    void *free; long long nfree, bump;   // bump: блоки с этого номера ещё ни разу не брали (там нули)
    struct tb_gpage *next, *prev, *anext;
} tb_gpage;
typedef struct { char *p; long long n; } tb_grange;
TB_API unsigned char tb_gcls[TB_GBIG / 8 + 1];
TB_API tb_gpage *tb_gall = 0, *tb_glarge = 0, *tb_gavail[2][TB_GNC];
// адрес → страница: таблица в два шага (биты 47..32, потом 31..16)
TB_API tb_gpage **tb_gdir[1 << 16];
TB_API uintptr_t tb_glo = ~(uintptr_t)0, tb_ghi = 0;
TB_API long long tb_gsince = 0, tb_glive = 0, tb_gnext = 8LL << 20, tb_gstress = 0, tb_gcount = 0, tb_gruns = 0;
TB_API int tb_gready = 0, tb_goff = 0, tb_gstats = 0;
TB_API char *tb_gtop = 0;
TB_API pthread_mutex_t tb_glock = PTHREAD_MUTEX_INITIALIZER;
TB_API tb_grange *tb_groots = 0;
TB_API long long tb_gnroots = 0, tb_gcaproots = 0;

TB_API void tb_gsetup(void) {
    int c = 0;
    for (long long i = 0; i <= TB_GBIG / 8; i++) {
        while (tb_gsz[c] < i * 8) c++;
        tb_gcls[i] = (unsigned char)c;
    }
    tb_gready = 1;
}
TB_INL tb_gpage *tb_gmap_get(uintptr_t a) {
    if (a >> 48) return 0;
    tb_gpage **l2 = tb_gdir[a >> 32];
    return l2 ? l2[(a >> TB_GSH) & 0xFFFF] : 0;
}
TB_API void tb_gmap_put(uintptr_t a, tb_gpage *pg) {
    tb_gpage ***d = &tb_gdir[a >> 32];
    if (!*d) {
        *d = calloc(1 << 16, sizeof(tb_gpage *));
        if (!*d) tb_crash(0, "не хватило памяти");
    }
    (*d)[(a >> TB_GSH) & 0xFFFF] = pg;
}
TB_API void tb_gmap_add(tb_gpage *pg, long long size) {
    for (long long o = 0; o < size; o += TB_GPG) tb_gmap_put((uintptr_t)pg->start + o, pg);
    if ((uintptr_t)pg->start < tb_glo) tb_glo = (uintptr_t)pg->start;
    if ((uintptr_t)pg->start + size > tb_ghi) tb_ghi = (uintptr_t)pg->start + size;
}
TB_API void tb_gmap_remove(tb_gpage *pg, long long size) {
    for (long long o = 0; o < size; o += TB_GPG) tb_gmap_put((uintptr_t)pg->start + o, 0);
}
// память у системы: mmap всегда отдаёт нули и выровнено по 64 КБ (лишнее по краям отдаём назад)
#ifdef _WIN32
// Windows: VirtualAlloc тоже даёт нули, и адрес у него всегда кратен 64 КБ — обрезать нечего.
// munmap здесь зовётся только для целого куска из tb_gsys — это VirtualFree(MEM_RELEASE).
TB_API char *tb_gsys(long long size) {
    char *m = VirtualAlloc(0, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!m) tb_crash(0, "не хватило памяти");
    return m;
}
#define munmap(p, n) (VirtualFree((p), 0, MEM_RELEASE) ? 0 : -1)
#else
#include <sys/mman.h>
#ifdef __linux__
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif
#ifndef MREMAP_FIXED
#define MREMAP_FIXED 2
#endif
extern void *mremap(void *old, size_t old_size, size_t new_size, int flags, ...);
#endif
TB_API char *tb_gsys(long long size) {
    size_t tot = (size_t)size + (size_t)TB_GPG;
    char *m = mmap(0, tot, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) tb_crash(0, "не хватило памяти");
    uintptr_t a = ((uintptr_t)m + TB_GPG - 1) & ~(uintptr_t)(TB_GPG - 1);
    if (a > (uintptr_t)m) munmap(m, a - (uintptr_t)m);
    size_t tail = (uintptr_t)m + tot - (a + (size_t)size);
    if (tail) munmap((char *)a + size, tail);
    return (char *)a;
}
#endif
// страницы по 64 КБ берём у системы пачкой по 4 МБ; пустые страницы возвращаются в запас
TB_API char *tb_gfresh = 0, *tb_gfresh_end = 0;
TB_API void *tb_gspare = 0;
TB_API char *tb_gpage_mem(void) {
    if (tb_gspare) { void *p = tb_gspare; tb_gspare = *(void **)p; *(void **)p = 0; return p; }
    if (tb_gfresh == tb_gfresh_end) { tb_gfresh = tb_gsys(64 * TB_GPG); tb_gfresh_end = tb_gfresh + 64 * TB_GPG; }
    char *p = tb_gfresh;
    tb_gfresh += TB_GPG;
    return p;
}
TB_API tb_gpage *tb_gnewpage(int cls, int np) {
    tb_gpage *pg = calloc(1, sizeof(tb_gpage));
    if (!pg) tb_crash(0, "не хватило памяти");
    pg->start = tb_gpage_mem();
    pg->cls = cls; pg->np = np; pg->bsize = tb_gsz[cls]; pg->nblocks = TB_GPG / pg->bsize;
    pg->inv = ((1ULL << 32) + pg->bsize - 1) / pg->bsize;
    pg->fl = calloc(pg->nblocks, 1);
    if (!pg->fl) tb_crash(0, "не хватило памяти");
    pg->free = 0; pg->bump = 0;
    pg->nfree = pg->nblocks;
    pg->next = tb_gall; tb_gall = pg;
    tb_gmap_add(pg, TB_GPG);
    pg->anext = tb_gavail[np][cls]; tb_gavail[np][cls] = pg; pg->in_avail = 1;
    return pg;
}
#define TB_GIDX(pg, off) ((long long)(((unsigned long long)(off) * (pg)->inv) >> 32))
// блок, внутрь которого указывает адрес a (или 0, если это не наша занятая память)
TB_INL char *tb_gblock(uintptr_t a, tb_gpage **out) {
    if (a < tb_glo || a >= tb_ghi) return 0;
    tb_gpage *pg = tb_gmap_get(a);
    if (!pg) return 0;
    if (pg->large) {
        if (a >= (uintptr_t)pg->start + (uintptr_t)pg->nbytes) return 0;
        *out = pg;
        return pg->start;
    }
    long long i = TB_GIDX(pg, a - (uintptr_t)pg->start);
    if (i >= pg->nblocks || !(pg->fl[i] & 1)) return 0;
    *out = pg;
    return pg->start + i * pg->bsize;
}
TB_API void tb_gc_collect(void);
TB_API void *tb_galloc_locked(long long n, int np) {
    if (!tb_gready) tb_gsetup();
    if (n <= 0) n = 1;
    // +1 байт: адрес «сразу за концом» (так иногда считает C) всё ещё внутри блока
    if (n >= 64) n += 1;
    if (TB_RARE(tb_gsince >= tb_gnext || tb_gstress) && !tb_goff && tb_gtop &&
        __atomic_load_n(&tb_gthreads, __ATOMIC_ACQUIRE) == 0 &&
        (tb_gsince >= tb_gnext || (tb_gstress && ++tb_gcount % tb_gstress == 0)))
        tb_gc_collect();
    if (n <= TB_GBIG) {
        int cls = tb_gcls[(n + 7) >> 3];
        tb_gpage *pg = tb_gavail[np][cls];
        if (TB_RARE(!pg)) pg = tb_gnewpage(cls, np);
        void **b;
        if (pg->free) {
            b = pg->free;
            pg->free = *b;
            *b = 0;
            pg->fl[TB_GIDX(pg, (char *)b - pg->start)] = 1;
        } else {
            b = (void **)(pg->start + pg->bump * pg->bsize);
            pg->fl[pg->bump++] = 1;
        }
        if (--pg->nfree == 0) { tb_gavail[np][cls] = pg->anext; pg->in_avail = 0; }
        tb_gsince += pg->bsize;
        return b;
    }
    long long cap = (n + TB_GPG - 1) & ~(TB_GPG - 1);
    tb_gpage *pg = calloc(1, sizeof(tb_gpage));
    if (!pg) tb_crash(0, "не хватило памяти");
    pg->start = tb_gsys(cap);
    pg->large = 1; pg->np = np; pg->nbytes = n; pg->cap = cap; pg->lfl = 1;
    pg->next = tb_glarge; if (tb_glarge) tb_glarge->prev = pg; tb_glarge = pg;
    tb_gmap_add(pg, cap);
    tb_gsince += cap;
    return pg->start;
}
TB_API void tb_gfree_locked(void *p) {
    tb_gpage *pg = 0;
    char *b = tb_gblock((uintptr_t)p, &pg);
    if (!b || b != (char *)p) return;   // не наша память (например, текст из кода) — не трогаем
    if (pg->large) {
        if (pg->prev) pg->prev->next = pg->next; else tb_glarge = pg->next;
        if (pg->next) pg->next->prev = pg->prev;
        tb_gmap_remove(pg, pg->cap);
        munmap(pg->start, pg->cap);
        free(pg);
        return;
    }
    memset(b, 0, pg->bsize);
    pg->fl[TB_GIDX(pg, b - pg->start)] = 0;
    *(void **)b = pg->free; pg->free = b; pg->nfree++;
    if (!pg->in_avail) { pg->anext = tb_gavail[pg->np][pg->cls]; tb_gavail[pg->np][pg->cls] = pg; pg->in_avail = 1; }
}
TB_INL int tb_gneed_lock(void) { return __atomic_load_n(&tb_gthreads, __ATOMIC_ACQUIRE) > 0; }
TB_INL void *tb_gc_alloc2(long long n, int np) {
    if (TB_RARE(tb_gneed_lock())) {
        pthread_mutex_lock(&tb_glock);
        void *p = tb_galloc_locked(n, np);
        pthread_mutex_unlock(&tb_glock);
        return p;
    }
    return tb_galloc_locked(n, np);
}
TB_API void *tb_gc_alloc(long long n) { return tb_gc_alloc2(n, 0); }
// память без адресов внутри (числа, тексты): уборка не читает её содержимое
TB_API void *tb_gc_alloc_np(long long n) { return tb_gc_alloc2(n, 1); }
TB_API void tb_gc_free(void *p) {
    if (!p) return;
    int lk = tb_gneed_lock();
    if (lk) pthread_mutex_lock(&tb_glock);
    tb_gfree_locked(p);
    if (lk) pthread_mutex_unlock(&tb_glock);
}
// вырастить блок: если места хватает — тот же блок, иначе новый, старый отдаём
TB_API void *tb_gc_realloc(void *p, long long old_n, long long new_n) {
    int lk = tb_gneed_lock();
    if (lk) pthread_mutex_lock(&tb_glock);
    tb_gpage *pg = 0;
    char *b = p ? tb_gblock((uintptr_t)p, &pg) : 0;
    int own = b != 0 && b == (char *)p;
    long long want = new_n >= 64 ? new_n + 1 : (new_n > 0 ? new_n : 1);
    if (own && pg->large && want <= pg->cap) {
        if (want > pg->nbytes) pg->nbytes = want;
        if (lk) pthread_mutex_unlock(&tb_glock);
        return p;
    }
    if (own && !pg->large && want <= pg->bsize) {
        if (lk) pthread_mutex_unlock(&tb_glock);
        return p;
    }
#ifdef __linux__
    // большой блок растёт без копирования: система переносит его страницы на новое место
    if (own && pg->large) {
        long long cap = (want + TB_GPG - 1) & ~(TB_GPG - 1);
        if (cap < 2 * pg->cap) cap = 2 * pg->cap;
        char *dst = tb_gsys(cap);
        void *r = mremap(pg->start, (size_t)pg->cap, (size_t)cap, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
        if (r != MAP_FAILED) {
            tb_gmap_remove(pg, pg->cap);
            pg->start = (char *)r; pg->cap = cap; pg->nbytes = want;
            tb_gmap_add(pg, cap);
            tb_gsince += cap / 2;
            if (lk) pthread_mutex_unlock(&tb_glock);
            return r;
        }
        munmap(dst, cap);
    }
#endif
    long long keep = old_n < new_n ? old_n : new_n;
    if (own) { long long room = pg->large ? pg->nbytes : pg->bsize; if (keep > room) keep = room; }
    void *q = tb_galloc_locked(new_n, own ? pg->np : 0);
    if (keep > 0 && p) memcpy(q, p, keep);
    if (own) tb_gfree_locked(p);
    if (lk) pthread_mutex_unlock(&tb_glock);
    return q;
}
// место, где программа хранит значения вне стека (общие переменные): обход смотрит и туда
TB_API void tb_gc_root(void *p, long long n) {
    if (tb_gnroots == tb_gcaproots) {
        tb_gcaproots = tb_gcaproots * 2 + 16;
        tb_groots = realloc(tb_groots, sizeof(tb_grange) * tb_gcaproots);
        if (!tb_groots) tb_crash(0, "не хватило памяти");
    }
    tb_groots[tb_gnroots].p = (char *)p;
    tb_groots[tb_gnroots].n = n;
    tb_gnroots++;
}
// очередь обхода
typedef struct { tb_grange *a; long long n, cap; } tb_gms;
TB_API _Thread_local tb_gms *tb_gcur = 0;
TB_API tb_gms tb_gmain_q = {0, 0, 0};
TB_INL void tb_gpush(tb_gms *q, char *p, long long n) {
    if (TB_RARE(q->n == q->cap)) {
        q->cap = q->cap * 2 + 1024;
        q->a = realloc(q->a, sizeof(tb_grange) * q->cap);
        if (!q->a) tb_crash(0, "не хватило памяти для уборки");
    }
    q->a[q->n].p = p;
    q->a[q->n].n = n;
    q->n++;
}
// последние страницы обхода: соседние адреса обычно ведут на ту же страницу — без поиска по таблице
TB_API _Thread_local tb_gpage *tb_glast[16];
TB_INL void tb_gmark(uintptr_t a) {
    tb_gpage *pg = tb_glast[(a >> TB_GSH) & 15];
    char *b;
    unsigned char *f;
    if (pg && a - (uintptr_t)pg->start < (uintptr_t)TB_GPG) {
        long long i = TB_GIDX(pg, a - (uintptr_t)pg->start);
        if (i >= pg->nblocks || !(pg->fl[i] & 1)) return;
        b = pg->start + i * pg->bsize;
        f = &pg->fl[i];
    } else {
        if (a < tb_glo || a >= tb_ghi) return;
        b = tb_gblock(a, &pg);
        if (!b) return;
        if (pg->large) f = &pg->lfl;
        else { f = &pg->fl[TB_GIDX(pg, b - pg->start)]; tb_glast[(a >> TB_GSH) & 15] = pg; }
    }
    if (*f & 2) return;
    *f |= 2;
    if (pg->np) return;
    tb_gpush(tb_gcur, b, pg->large ? pg->nbytes : pg->bsize);
    __builtin_prefetch(b);
}
TB_API void tb_gscan(const char *lo, const char *hi) {
    uintptr_t p = ((uintptr_t)lo + 7) & ~(uintptr_t)7;
    for (; p + 8 <= (uintptr_t)hi; p += 8) tb_gmark(*(const uintptr_t *)p);
}
// одна запись очереди: большой блок читаем кусками по 4 КБ, остаток кладём обратно,
// а найденное в куске обходим сразу — так очередь остаётся маленькой
TB_INL void tb_gstep(tb_gms *q) {
    tb_grange r = q->a[--q->n];
    if (r.n > 4096) {
        q->a[q->n].p = r.p + 4096;
        q->a[q->n].n = r.n - 4096;
        q->n++;
        r.n = 4096;
    }
    tb_gscan(r.p, r.p + r.n);
}
TB_API void tb_gdrain(void) {
    tb_gms *q = tb_gcur;
    while (q->n > 0) tb_gstep(q);
}
__attribute__((noinline)) TB_API void tb_gscan_stack(void) {
    volatile char here = 0;
    tb_gscan((const char *)&here, tb_gtop);
}
__attribute__((noinline)) TB_API void tb_gc_run(void) {
    memset(tb_glast, 0, sizeof tb_glast);
    tb_gcur = &tb_gmain_q;
    // регистры процессора — на стек: в них тоже могут быть нужные адреса
    __builtin_unwind_init();
    jmp_buf jb;
    setjmp(jb);
    tb_gscan((const char *)&jb, (const char *)&jb + sizeof jb);
    tb_gscan_stack();
    tb_gdrain();
    for (long long i = 0; i < tb_gnroots; i++) { tb_gscan(tb_groots[i].p, tb_groots[i].p + tb_groots[i].n); tb_gdrain(); }
    for (int i = 0; i < tb_nchunks; i++) {
        // временная память может быть большой: тоже кусками
        for (long long o = 0; o < tb_chunks[i].used; o += 4096) {
            long long e = o + 4096 < tb_chunks[i].used ? o + 4096 : tb_chunks[i].used;
            tb_gscan(tb_chunks[i].mem + o, tb_chunks[i].mem + e);
            tb_gdrain();
        }
    }
    tb_gdrain();
    memset(tb_glast, 0, sizeof tb_glast);
}
TB_API double tb_gnow(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
TB_API void tb_gc_collect(void) {
    tb_gruns++;
    double t0 = tb_gstats ? tb_gnow() : 0;
    tb_gc_run();
    double t1 = tb_gstats ? tb_gnow() : 0;
    // всё не помеченное — мусор
    long long live = 0;
    int keep_empty[TB_GNC];
    for (int c = 0; c < TB_GNC; c++) { tb_gavail[0][c] = 0; tb_gavail[1][c] = 0; keep_empty[c] = 2; }
    tb_gpage **pp = &tb_gall;
    while (*pp) {
        tb_gpage *pg = *pp;
        long long used = 0;
        pg->free = 0; pg->nfree = pg->nblocks - pg->bump;
        long long i = pg->bump - 1;
        // быстрый путь: 8 подряд занятых и нужных блоков — просто снимаем отметки
        long long full = 0;
        for (long long k = 0; k + 8 <= pg->bump; k += 8) {
            unsigned long long w;
            memcpy(&w, pg->fl + k, 8);
            if (w != 0x0303030303030303ULL) break;
            w = 0x0101010101010101ULL;
            memcpy(pg->fl + k, &w, 8);
            full = k + 8;
        }
        used += full;
        for (; i >= full; i--) {
            char *b = pg->start + i * pg->bsize;
            if (pg->fl[i] == 3) { pg->fl[i] = 1; used++; continue; }
            if (pg->fl[i] & 1) memset(b, 0, pg->bsize);
            pg->fl[i] = 0;
            *(void **)b = pg->free; pg->free = b; pg->nfree++;
        }
        if (used == 0 && keep_empty[pg->cls] == 0) {
            *pp = pg->next;
            tb_gmap_remove(pg, TB_GPG);
            // страница — в запас, чистой (в свободных блоках лежали ссылки на соседей)
            memset(pg->start, 0, pg->bump * pg->bsize);
            *(void **)pg->start = tb_gspare; tb_gspare = pg->start;
            free(pg->fl); free(pg);
            continue;
        }
        if (used == 0) keep_empty[pg->cls]--;
        live += used * pg->bsize;
        pg->in_avail = pg->nfree > 0;
        if (pg->in_avail) { pg->anext = tb_gavail[pg->np][pg->cls]; tb_gavail[pg->np][pg->cls] = pg; }
        pp = &pg->next;
    }
    tb_gpage *pg = tb_glarge;
    while (pg) {
        tb_gpage *nx = pg->next;
        if (!(pg->lfl & 2)) {
            if (pg->prev) pg->prev->next = pg->next; else tb_glarge = pg->next;
            if (pg->next) pg->next->prev = pg->prev;
            tb_gmap_remove(pg, pg->cap);
            munmap(pg->start, pg->cap);
            free(pg);
        } else {
            pg->lfl = 1;
            live += pg->cap;
        }
        pg = nx;
    }
    tb_lc_clear();   // длины текстов по адресу: адрес мог достаться новому тексту
    long long before = tb_glive + tb_gsince;
    tb_glive = live;
    tb_gsince = 0;
    // следующий обход — когда новой памяти станет вдвое больше нужной
    // если мусора почти не нашлось (программа копит данные), следующий обход — ещё позже
    // если мусора почти не нашлось (программа копит данные) два раза подряд — ещё реже (×8),
    // но лишней памяти сверх «вдвое» не больше 512 МБ
    static int tb_gkeep = 0;
    tb_gkeep = live * 10 > before * 9 ? tb_gkeep + 1 : 0;
    long long k = tb_gkeep >= 2 ? 8 : (live * 4 > before * 3 ? 4 : 2);
    long long next = k * live;
    long long cap = 2 * live > live + (512LL << 20) ? 2 * live : live + (512LL << 20);
    if (next > cap) next = cap;
    tb_gnext = next > (8LL << 20) ? next : (8LL << 20);
    if (tb_gstats) fprintf(stderr, "[уборка %lld: нужно %lld МБ, было %lld МБ, обход %.1f мс, чистка %.1f мс]\n",
        tb_gruns, live >> 20, before >> 20, t1 - t0, tb_gnow() - t1);
}
// начало программы: top — адрес в кадре main, выше всех кадров программы
// main программы зовём через этот шаг (не встраивается): её переменные точно ниже tb_top
__attribute__((noinline)) TB_API void tb_call_main(void (*f)(void)) {
    void (*volatile g)(void) = f;
    g();
}
TB_API void tb_gc_init(void *top) {
    tb_gtop = (char *)top;
    const char *e = getenv("TB_GC");
    if (e && e[0] == '0') tb_goff = 1;
    e = getenv("TB_GC_STATS");
    if (e && e[0] == '1') tb_gstats = 1;
    e = getenv("TB_GC_STRESS");
    if (e) tb_gstress = atoll(e);
}
#endif
TB_API void *tb_mem(long long n) {
    if (tb_temp_depth > 0) return tb_temp_alloc(n);
    return tb_gc_alloc(n);
}
// то же для памяти без адресов внутри: текст, числа
TB_API void *tb_mem_np(long long n) {
    if (tb_temp_depth > 0) return tb_temp_alloc(n);
    return tb_gc_alloc_np(n);
}
#define tb_tmem tb_mem_np
// вырасти кусок: временный текущей области растёт там же, обычный — через realloc.
// Кусок из внешней области (список создан снаружи, растёт внутри temp или шага цикла)
// переезжает в обычную память: внутренняя область скоро отдаётся, а список живёт дольше.
TB_API void *tb_resize(void *p, long long old_n, long long new_n) {
    tb_lc_clear();
    if (tb_nchunks > 0 && tb_in_temp(p)) {
        void *q = tb_in_cur_temp(p) ? tb_temp_alloc(new_n) : tb_gc_alloc(new_n);
        memcpy(q, p, old_n);
        return q;
    }
    return tb_gc_realloc(p, old_n, new_n);
}
TB_API tb_list *tb_list_new(long long size) {
    tb_list *l = tb_mem(sizeof(tb_list));
    l->len = 0; l->cap = 4; l->size = size; l->data = tb_mem(size * 4);
    return l;
}
TB_API tb_list *tb_list_new_np(long long size) {
    tb_list *l = tb_mem(sizeof(tb_list));
    l->len = 0; l->cap = 4; l->size = size; l->data = tb_mem_np(size * 4);
    return l;
}
TB_API tb_list *tb_list_from_np(long long n, long long size, const void *src) {
    tb_list *l = tb_mem(sizeof(tb_list));
    l->len = n; l->cap = n; l->size = size; l->data = tb_mem_np(size * n);
    memcpy(l->data, src, size * n);
    return l;
}
TB_API tb_list *tb_list_from(long long n, long long size, const void *src) {
    tb_list *l = tb_mem(sizeof(tb_list));
    l->len = n; l->cap = n; l->size = size; l->data = tb_mem(size * n);
    memcpy(l->data, src, size * n);
    return l;
}
// size передаётся как sizeof(T): после встраивания C знает его заранее
TB_INL void *tb_at(tb_list *l, long long i, long long size, int line) {
    // одно сравнение вместо двух: отрицательный номер как unsigned становится огромным
    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, "номер за пределами списка");
    return l->data + i * size;
}
TB_INL void *tb_grow(tb_list *l) {
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
TB_API void *tb_pop(tb_list *l, int line) {
    if (l->len == 0) tb_crash(line, ".pop() у пустого списка");
    l->len -= 1;
    return l->data + l->len * l->size;
}
// ---------- list bool: по одному биту на элемент (в 8 раз меньше памяти) ----------
// тот же tb_list: len и cap — в битах, size = 0, data — слова по 64 бита
TB_API tb_list *tb_bits_new(void) {
    tb_list *l = tb_mem(sizeof(tb_list));
    l->len = 0; l->cap = 64; l->size = 0; l->data = tb_mem_np(8);
    memset(l->data, 0, 8);
    return l;
}
TB_API tb_list *tb_bits_from(long long n, const unsigned char *src) {
    tb_list *l = tb_mem(sizeof(tb_list));
    long long words = (n + 63) / 64 + (n == 0);
    l->len = n; l->cap = words * 64; l->size = 0; l->data = tb_mem_np(words * 8);
    memset(l->data, 0, words * 8);
    unsigned long long *w = (unsigned long long *)l->data;
    for (long long i = 0; i < n; i++) if (src[i]) w[i >> 6] |= 1ULL << (i & 63);
    return l;
}
// бит без проверки номера (для each — номер всегда внутри)
#define TB_BIT(l, i) ((_Bool)((((unsigned long long *)(l)->data)[(i) >> 6] >> ((i) & 63)) & 1))
TB_INL _Bool tb_bget(tb_list *l, long long i, int line) {
    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, "номер за пределами списка");
    return TB_BIT(l, i);
}
TB_INL void tb_bset(tb_list *l, long long i, _Bool v, int line) {
    if (TB_RARE((unsigned long long)i >= (unsigned long long)l->len)) tb_crash(line, "номер за пределами списка");
    unsigned long long *w = (unsigned long long *)l->data + (i >> 6), m = 1ULL << (i & 63);
    *w = v ? (*w | m) : (*w & ~m);
}
TB_INL void tb_badd(tb_list *l, _Bool v) {
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
TB_INL _Bool tb_bpop(tb_list *l, int line) {
    if (l->len == 0) tb_crash(line, ".pop() у пустого списка");
    l->len -= 1;
    return TB_BIT(l, l->len);
}
// N одинаковых битов в конец (repeat N times: flags.add(true))
TB_API void tb_bfill(tb_list *l, long long n, _Bool v) {
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
TB_API tb_list *tb_bits_of_bytes(tb_list *b) { return tb_bits_from(b->len, (const unsigned char *)b->data); }
// N одинаковых элементов в конец (repeat N times: nums.add(0))
TB_API void tb_fill(tb_list *l, long long n, const void *v) {
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
TB_API void *tb_nn(void *p, int line) {
    if (p == 0) tb_crash(line, "obj пустой: его ещё не создали");
    return p;
}
TB_API const char *tb_cat(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *s = tb_tmem(la + lb + 1);
    memcpy(s, a, la);
    memcpy(s + la, b, lb + 1);
    return s;
}
TB_API const char *tb_itos(long long x) { char *s = tb_tmem(32); snprintf(s, 32, "%lld", x); return s; }
TB_API const char *tb_dtos(double x) { char *s = tb_tmem(40); snprintf(s, 40, "%g", x); return s; }
TB_API const char *tb_btos(int x) { return x ? "true" : "false"; }
// текст из одного байта: 256 готовых строк, без выделения памяти на каждую букву
// таблица постоянная: компилятор C видит, что второй байт всегда 0, и  ch == ","  становится одним сравнением байта
TB_API const char tb_chars[512] = {
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
TB_INL const char *tb_chr(long long c) { return tb_chars + 2 * (unsigned char)c; }
TB_INL long long tb_tlen(const char *s) {
    if (tb_lc_p[0] == s) return tb_lc_n[0];
    if (tb_lc_p[1] == s) return tb_lc_n[1];
    long long n = (long long)strlen(s);
    tb_lc_p[tb_lc_next] = s; tb_lc_n[tb_lc_next] = n; tb_lc_next ^= 1;
    return n;
}
// n — длина, если компилятор её знает (текст, в который дописывают), иначе -1
TB_INL const char *tb_char_at(const char *s, long long i, long long n, int line) {
    if (n < 0) n = tb_tlen(s);
    if (i < 0 || i >= n) tb_crash(line, "номер за пределами текста");
    return tb_chr((unsigned char)s[i]);
}
// s += b: дописать в конец своей памяти s, а не собирать третий текст из двух.
// *rl — длина s в своей памяти (-1: своей памяти нет, s могли сохранить где-то ещё — сначала копия),
// *rc — сколько там места.
TB_API void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb);
// число цифрами в конец буфера d[24]: вернёт начало цифр, длину — в *n
TB_INL const char *tb_digits(long long x, char *d, long long *n) {
    unsigned long long u = x < 0 ? 0ULL - (unsigned long long)x : (unsigned long long)x;
    char *p = d + 24;
    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);
    if (x < 0) *--p = '-';
    *n = d + 24 - p;
    return p;
}
TB_INL void tb_app(const char **p, long long *rl, long long *rc, const char *b) {
    tb_app_n(p, rl, rc, b, (long long)strlen(b));
}
// дописать число: цифры пишутся прямо в конец, без отдельного текста
TB_INL void tb_app_i(const char **p, long long *rl, long long *rc, long long x) {
    char d[24]; int n = 0;
    unsigned long long u = x < 0 ? 0ULL - (unsigned long long)x : (unsigned long long)x;
    do { d[23 - n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (x < 0) d[23 - n++] = '-';
    tb_app_n(p, rl, rc, d + 24 - n, n);
}
TB_API void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb);
// ядро отдаёт свой кусок в общий текст: если общий ещё пустой — забирает память куска целиком, без копии
TB_API void tb_app_take(const char **p, long long *rl, long long *rc, const char *b, long long brl, long long brc) {
    if (*rl < 0 && (*p)[0] == 0 && brl >= 0) { *p = b; *rl = brl; *rc = brc; return; }
    tb_app_n(p, rl, rc, b, brl >= 0 ? brl : (long long)strlen(b));
}
TB_API void tb_app_n(const char **p, long long *rl, long long *rc, const char *b, long long lb) {
    char *s = (char *)*p;
    if (*rl < 0) {
        long long la = (long long)strlen(s);
        long long cap = 2 * (la + lb) + 1;
        if (cap < 64) cap = 64;
        char *r = tb_tmem(cap);
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
TB_API const char *tb_cut(const char *s, long long from, long long to, int line) {
    long long n = (long long)strlen(s);
    if (from < 0 || to >= n || from > to + 1) tb_crash(line, "cut: номера за пределами текста");
    char *r = tb_tmem(to - from + 2);
    memcpy(r, s + from, to - from + 1);
    r[to - from + 1] = 0;
    return r;
}
TB_API long long tb_code(const char *s, int line) {
    if (s[0] == 0) tb_crash(line, "code: текст пустой");
    return (unsigned char)s[0];
}
TB_API long long tb_div(long long a, long long b, int line) { if (b == 0) tb_crash(line, "деление на ноль"); return a / b; }
TB_API long long tb_mod(long long a, long long b, int line) { if (b == 0) tb_crash(line, "остаток от деления на ноль"); return a % b; }
TB_API long long tb_round(double x) { return (long long)(x < 0 ? x - 0.5 : x + 0.5); }
TB_API void tb_ps(const char *s) { fputs(s, stdout); }



// ---------- ошибки fail / try / or / else err ----------
// Функция, которая сломалась, кладёт текст ошибки в tb_err и сразу выходит.
// Тот, кто её вызвал, смотрит в tb_err.
TB_API _Thread_local const char *tb_err = 0;   // у каждого ядра своя

TB_COLD TB_API void tb_die(const char *msg) {
    fflush(stdout);
    fprintf(stderr, "ошибка: %s\n", msg);
    exit(1);
}

// ---------- текст ----------
TB_API const char *tb_upper(const char *s) {
    size_t n = strlen(s); char *r = tb_tmem(n + 1);
    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
    return r;
}
TB_API const char *tb_lower(const char *s) {
    size_t n = strlen(s); char *r = tb_tmem(n + 1);
    for (size_t i = 0; i <= n; i++) r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];
    return r;
}
TB_API int tb_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
TB_API const char *tb_trim(const char *s) {
    size_t a = 0, b = strlen(s);
    while (a < b && tb_space(s[a])) a++;
    while (b > a && tb_space(s[b - 1])) b--;
    char *r = tb_tmem(b - a + 1); memcpy(r, s + a, b - a); r[b - a] = 0;
    return r;
}
TB_API long long tb_find(const char *s, const char *part) {
    const char *p = strstr(s, part);
    return p ? (long long)(p - s) : -1;
}
TB_API int tb_starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
TB_API int tb_ends(const char *s, const char *p) {
    size_t a = strlen(s), b = strlen(p);
    return b <= a && strcmp(s + a - b, p) == 0;
}
TB_API const char *tb_replace(const char *s, const char *from, const char *to, int line) {
    size_t lf = strlen(from), lt = strlen(to), count = 0;
    if (lf == 0) tb_crash(line, "replace: что заменять — пустой текст");
    for (const char *p = strstr(s, from); p; p = strstr(p + lf, from)) count++;
    char *r = tb_tmem(strlen(s) + count * lt + 1), *w = r;
    const char *p = s, *q;
    while ((q = strstr(p, from)) != 0) {
        memcpy(w, p, q - p); w += q - p;
        memcpy(w, to, lt); w += lt;
        p = q + lf;
    }
    strcpy(w, p);
    return r;
}
TB_API tb_list *tb_split(const char *s, const char *sep, int line) {
    size_t ls = strlen(sep);
    if (ls == 0) tb_crash(line, "split: разделитель — пустой текст");
    tb_list *l = tb_list_new(sizeof(const char *));
    const char *p = s, *q;
    while (1) {
        q = strstr(p, sep);
        size_t n = q ? (size_t)(q - p) : strlen(p);
        char *part = tb_tmem(n + 1); memcpy(part, p, n); part[n] = 0;
        *(const char **)tb_grow(l) = part;
        if (!q) break;
        p = q + ls;
    }
    return l;
}
TB_API const char *tb_join(tb_list *l, const char *sep) {
    size_t n = 0, ls = strlen(sep);
    for (long long i = 0; i < l->len; i++) n += strlen(((const char **)l->data)[i]) + ls;
    char *r = tb_tmem(n + 1), *w = r;
    for (long long i = 0; i < l->len; i++) {
        const char *part = ((const char **)l->data)[i];
        if (i > 0) { memcpy(w, sep, ls); w += ls; }
        memcpy(w, part, strlen(part)); w += strlen(part);
    }
    *w = 0;
    return r;
}
TB_API long long tb_to_int(const char *s) {
    long long v = 0; int i = 0, minus = 0;
    if (s[0] == '-') { minus = 1; i = 1; }
    if (s[i] == 0) { tb_err = tb_cat("это не число: ", s); return 0; }
    for (; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') { tb_err = tb_cat("это не число: ", s); return 0; }
        v = v * 10 + (s[i] - '0');
    }
    return minus ? -v : v;
}
TB_API double tb_to_dec(const char *s) {
    char *end = 0;
    double v = strtod(s, &end);
    if (s[0] == 0 || *end != 0) { tb_err = tb_cat("это не дробное число: ", s); return 0; }
    return v;
}

// ---------- файлы, ввод, слова запуска ----------
TB_API const char *tb_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == 0) { tb_err = tb_cat("не могу открыть файл: ", path); return ""; }
    size_t cap = 4096, n = 0, got;
    char *buf = tb_tmem(cap);
    while ((got = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += got;
        if (n + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }
    }
    fclose(f);
    buf[n] = 0;
    return buf;
}
TB_API void tb_write_file(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (f == 0) { tb_err = tb_cat("не могу записать файл: ", path); return; }
    fputs(s, f);
    fclose(f);
}
TB_API const char *tb_input(void) {
    fflush(stdout);
    size_t cap = 256, n = 0;
    char *buf = tb_tmem(cap);
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
TB_API tb_list *tb_args;
TB_API long long tb_run(const char *command) {
    fflush(stdout);
#ifdef _WIN32
    // на Windows команду выполняет cmd.exe, а system сразу отдаёт код выхода
    fflush(stderr);
    return tb_win_system(command);
#else
    int code = system(command);
    if (code == -1) return -1;
    return code == 0 ? 0 : (code > 255 ? code / 256 : code);
#endif
}
TB_API void tb_exit(long long code) { fflush(stdout); exit((int)code); }
TB_API int tb_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == 0) return 0;
    fclose(f);
    return 1;
}
// ---------- папки (в ядре без ОС их нет) ----------
#ifndef TB_BARE
TB_API int tb_name_cmp(const void *a, const void *b) {
    const char *x = *(const char * const *)a, *y = *(const char * const *)b;
    size_t lx = strlen(x), ly = strlen(y);
    int dx = lx > 0 && x[lx - 1] == '/', dy = ly > 0 && y[ly - 1] == '/';
    if (dx != dy) return dy - dx;          // папки первыми
    return strcasecmp(x, y);
}
TB_API int tb_is_folder(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
TB_API tb_list *tb_list_dir(const char *path) {
    tb_list *l = tb_list_new(sizeof(const char *));
    DIR *d = opendir(path);
    if (d == 0) { tb_err = tb_cat("не могу открыть папку: ", path); return l; }
    struct dirent *e;
    size_t lp = strlen(path);
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        size_t n = strlen(e->d_name);
        char *full = tb_tmem(lp + n + 2);
        memcpy(full, path, lp); full[lp] = '/'; memcpy(full + lp + 1, e->d_name, n + 1);
        int dir = tb_is_folder(full);
        char *name = tb_tmem(n + 2);
        memcpy(name, e->d_name, n);
        if (dir) name[n++] = '/';
        name[n] = 0;
        *(const char **)tb_grow(l) = name;
    }
    closedir(d);
    if (l->len > 1) qsort(l->data, l->len, sizeof(const char *), tb_name_cmp);
    return l;
}
TB_API void tb_make_folder(const char *path) {
    if (mkdir(path, 0755) != 0) tb_err = tb_cat(errno == EEXIST ? "уже есть: " : "не могу создать папку: ", path);
}
TB_API void tb_move(const char *from, const char *to) {
    if (rename(from, to) != 0) tb_err = tb_cat(errno == EXDEV ? "нельзя переместить на другой диск: " : "не могу переместить: ", from);
}
TB_API void tb_remove(const char *path) {
    int r = tb_is_folder(path) ? rmdir(path) : unlink(path);
    if (r != 0) tb_err = tb_cat(errno == ENOTEMPTY || errno == EEXIST ? "папка не пустая: " : "не могу удалить: ", path);
}
TB_API const char *tb_env(const char *name) {
    const char *v = getenv(name);
    return v ? v : "";
}
TB_API long long tb_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long)st.st_size;
}

// ---------- время и дата ----------
TB_API long long tb_unix_time(void) { return (long long)time(0); }
TB_API const char *tb_date_now(void) {
    time_t t = time(0);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char *s = tb_tmem(32);
    strftime(s, 32, "%Y-%m-%d %H:%M:%S", &tmv);
    return s;
}

// ---------- http_get: страница из интернета через curl (есть на Mac и почти на любом Linux) ----------
TB_API const char *tb_http_get(const char *url) {
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        tb_err = tb_cat("адрес должен начинаться с http:// или https://: ", url);
        return "";
    }
    // адрес — в одинарных кавычках для shell: ' внутри адреса пишется как '\''
    size_t n = strlen(url);
    char *cmd = malloc(n * 4 + 80);
    if (!cmd) { tb_err = "не хватило памяти"; return ""; }
#ifdef _WIN32
    // curl.exe есть в Windows 10 и новее; адрес — в двойных кавычках для cmd.exe,
    // (внутри кавычек & | < > ^ для cmd — простые знаки), а " внутри адреса — как %22
    size_t k = (size_t)sprintf(cmd, "curl.exe -fsSL --max-time 30 \"");
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)url[i];
        if (ch == '"' || ch < 32) { sprintf(cmd + k, "%%%02X", ch); k += 3; }
        else cmd[k++] = url[i];
    }
    strcpy(cmd + k, "\" 2>NUL");
    FILE *p = popen(cmd, "rb");
#else
    size_t k = (size_t)sprintf(cmd, "curl -fsSL --max-time 30 '");
    for (size_t i = 0; i < n; i++) {
        if (url[i] == '\'') { memcpy(cmd + k, "'\\''", 4); k += 4; }
        else cmd[k++] = url[i];
    }
    strcpy(cmd + k, "' 2>/dev/null");
    FILE *p = popen(cmd, "r");
#endif
    free(cmd);
    if (p == 0) { tb_err = "не могу запустить curl"; return ""; }
    size_t cap = 4096, len = 0, got;
    char *buf = tb_tmem(cap);
    while ((got = fread(buf + len, 1, cap - len - 1, p)) > 0) {
        len += got;
        if (len + 1 == cap) { buf = tb_resize(buf, cap, cap * 2); cap *= 2; }
    }
    buf[len] = 0;
    if (pclose(p) != 0) { tb_err = tb_cat("не получилось скачать (нет сети, нет curl или сервер ответил ошибкой): ", url); return ""; }
    return buf;
}

// ---------- JSON: json_get(текст, "users.0.name") и json_text(текст) ----------
TB_API const char *tb_js_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }
// пропустить одно значение; адрес после него или 0, если JSON сломан
TB_API const char *tb_js_skip(const char *p) {
    p = tb_js_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        return *p ? p + 1 : 0;
    }
    if (*p == '{' || *p == '[') {
        int d = 0;
        while (*p) {
            if (*p == '"') { p = tb_js_skip(p); if (!p) return 0; continue; }
            if (*p == '{' || *p == '[') d++;
            else if (*p == '}' || *p == ']') { d--; if (d == 0) return p + 1; }
            p++;
        }
        return 0;
    }
    const char *s = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p == s ? 0 : p;
}
TB_API int tb_js_hex(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = p[i]; v <<= 4;
        if (ch >= '0' && ch <= '9') v |= ch - '0';
        else if (ch >= 'a' && ch <= 'f') v |= ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v |= ch - 'A' + 10;
        else return -1;
    }
    return v;
}
// строка JSON (p смотрит на ") → обычный текст
TB_API const char *tb_js_str(const char *p) {
    const char *e = tb_js_skip(p);
    if (!e) return 0;
    char *out = tb_tmem(e - p + 1), *o = out;
    p++;
    while (*p != '"') {
        if (*p != '\\') { *o++ = *p++; continue; }
        p++;
        char ch = *p++;
        if (ch == 'n') *o++ = '\n'; else if (ch == 't') *o++ = '\t'; else if (ch == 'r') *o++ = '\r';
        else if (ch == 'b') *o++ = '\b'; else if (ch == 'f') *o++ = '\f';
        else if (ch == 'u') {
            int cp = tb_js_hex(p);
            if (cp < 0) return 0;
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {
                int lo = tb_js_hex(p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
            }
            if (cp < 0x80) *o++ = (char)cp;
            else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 63)); *o++ = (char)(0x80 | (cp & 63)); }
            else { *o++ = (char)(0xF0 | (cp >> 18)); *o++ = (char)(0x80 | ((cp >> 12) & 63)); *o++ = (char)(0x80 | ((cp >> 6) & 63)); *o++ = (char)(0x80 | (cp & 63)); }
        }
        else *o++ = ch;
    }
    *o = 0;
    return out;
}
TB_API const char *tb_json_get(const char *js, const char *path) {
    const char *p = tb_js_ws(js), *seg = path;
    while (*seg) {
        char key[256];
        size_t k = 0;
        while (seg[k] && seg[k] != '.' && k < 255) { key[k] = seg[k]; k++; }
        key[k] = 0;
        seg += k;
        if (*seg == '.') seg++;
        p = tb_js_ws(p);
        if (*p == '{') {
            p++;
            int found = 0;
            for (;;) {
                p = tb_js_ws(p);
                if (*p == '}') break;
                if (*p != '"') goto bad;
                const char *name = tb_js_str(p);
                p = tb_js_skip(p);
                if (!name || !p) goto bad;
                p = tb_js_ws(p);
                if (*p != ':') goto bad;
                p = tb_js_ws(p + 1);
                if (strcmp(name, key) == 0) { found = 1; break; }
                p = tb_js_skip(p);
                if (!p) goto bad;
                p = tb_js_ws(p);
                if (*p == ',') { p++; continue; }
                if (*p == '}') break;
                goto bad;
            }
            if (!found) { tb_err = tb_cat("в JSON нет ключа: ", key); return ""; }
        } else if (*p == '[') {
            char *end;
            long long want = strtoll(key, &end, 10);
            if (key[0] == 0 || *end != 0 || want < 0) { tb_err = tb_cat("у списка в JSON номер — число, а тут: ", key); return ""; }
            p++;
            for (long long i = 0;; i++) {
                p = tb_js_ws(p);
                if (*p == ']') { tb_err = tb_cat("в списке JSON нет номера ", key); return ""; }
                if (i == want) break;
                p = tb_js_skip(p);
                if (!p) goto bad;
                p = tb_js_ws(p);
                if (*p == ',') p++;
                else if (*p != ']') goto bad;
            }
        } else {
            tb_err = tb_cat("тут в JSON не объект и не список, а дальше в пути ещё: ", key);
            return "";
        }
    }
    p = tb_js_ws(p);
    if (*p == '"') { const char *s = tb_js_str(p); if (!s) goto bad; return s; }
    {
        const char *e = tb_js_skip(p);
        if (!e) goto bad;
        char *out = tb_tmem(e - p + 1);
        memcpy(out, p, e - p);
        out[e - p] = 0;
        return out;
    }
bad:
    tb_err = "JSON сломан: не могу разобрать";
    return "";
}
// текст → строка JSON в кавычках: "a\"b"
TB_API const char *tb_json_text(const char *s) {
    size_t n = strlen(s);
    char *out = tb_tmem(n * 6 + 3), *o = out;
    *o++ = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { *o++ = '\\'; *o++ = (char)*p; }
        else if (*p == '\n') { *o++ = '\\'; *o++ = 'n'; }
        else if (*p == '\r') { *o++ = '\\'; *o++ = 'r'; }
        else if (*p == '\t') { *o++ = '\\'; *o++ = 't'; }
        else if (*p < 32) o += sprintf(o, "\\u%04x", *p);
        else *o++ = (char)*p;
    }
    *o++ = '"';
    *o = 0;
    return out;
}
// ---------- время и случайные числа ----------
TB_API long long tb_time_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
TB_API void tb_wait_ms(long long n) { fflush(stdout); if (n > 0) usleep((useconds_t)(n * 1000)); }
TB_API unsigned long long tb_rand_state = 0;
// цвет из красного, зелёного и синего (0..255) — одно число 0xRRGGBB
TB_API long long tb_rgb(long long r, long long g, long long b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (r << 16) | (g << 8) | b;
}
// случайное целое от a до b (оба входят)
TB_API long long tb_random(long long a, long long b) {
    if (tb_rand_state == 0) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        tb_rand_state = ((unsigned long long)ts.tv_nsec << 20) ^ (unsigned long long)ts.tv_sec ^ 0x9E3779B97F4A7C15ULL;
    }
    if (b < a) { long long t = a; a = b; b = t; }
    tb_rand_state ^= tb_rand_state << 13; tb_rand_state ^= tb_rand_state >> 7; tb_rand_state ^= tb_rand_state << 17;
    unsigned long long span = (unsigned long long)(b - a) + 1;
    return span == 0 ? (long long)tb_rand_state : a + (long long)(tb_rand_state % span);
}
#endif

// ---------- числа ----------
TB_API long long tb_absi(long long x) { return x < 0 ? -x : x; }
TB_API long long tb_mini(long long a, long long b) { return a < b ? a : b; }
TB_API long long tb_maxi(long long a, long long b) { return a > b ? a : b; }
TB_API double tb_mind(double a, double b) { return a < b ? a : b; }
TB_API double tb_maxd(double a, double b) { return a > b ? a : b; }
TB_API double tb_sqrt(double x, int line) { if (x < 0) tb_crash(line, "sqrt: корень из отрицательного числа"); return sqrt(x); }

// ---------- sort ----------
TB_API int tb_cmp_int(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return (x > y) - (x < y); }
TB_API int tb_cmp_dec(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
TB_API int tb_cmp_text(const void *a, const void *b) { return strcmp(*(const char * const *)a, *(const char * const *)b); }
TB_API tb_list *tb_copy(tb_list *l) {
    if (l->size == 0) {
        tb_list *r = tb_mem(sizeof(tb_list));
        long long bytes = l->cap / 8;
        r->len = l->len; r->cap = l->cap; r->size = 0; r->data = tb_mem_np(bytes);
        memcpy(r->data, l->data, bytes);
        return r;
    }
    return tb_list_from(l->len, l->size, l->data);
}
TB_API void tb_sort_list(tb_list *l, int (*cmp)(const void *, const void *)) { qsort(l->data, l->len, l->size, cmp); }

// ---------- dict: ключи и значения лежат в двух списках по порядку добавления,
// а найти номер по ключу можно тремя способами (mode):
//   1 — «номер = адрес»: ключи int шли подряд base, base+1, base+2 ... — номер = ключ - base, без поиска;
//   2 — «адресное хранение»: ключи int лежат плотно — pos[ключ - base] = номер + 1, один взгляд;
//   0 — таблица slots с хешем (текст или разбросанные числа) ----------
typedef struct {
    tb_list *keys; tb_list *vals; long long *slots; long long nslots; int text;
    int mode; long long base; long long *pos; long long npos;
} tb_dict;

// ячейка таблицы: номер ключа + 1 (младшие 40 бит) и метка из старших битов хеша
#define TB_DAT ((1LL << 40) - 1)
#define TB_DTAG(h) ((long long)(((h) >> 40) << 40) & ~TB_DAT & 0x7FFFFFFFFFFFFFFFLL)
TB_API unsigned long long tb_hash(tb_dict *d, const void *k) {
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
TB_API int tb_keq(tb_dict *d, const void *a, const void *b) {
    if (d->text) return strcmp(*(const char * const *)a, *(const char * const *)b) == 0;
    return *(const long long *)a == *(const long long *)b;
}
TB_INL long long tb_dfind(tb_dict *d, const void *k) {
    if (d->mode == 1) {
        unsigned long long at = (unsigned long long)(*(const long long *)k - d->base);
        return at < (unsigned long long)d->keys->len ? (long long)at : -1;
    }
    if (d->mode == 2) {
        unsigned long long o = (unsigned long long)(*(const long long *)k - d->base);
        return o < (unsigned long long)d->npos ? d->pos[o] - 1 : -1;
    }
    unsigned long long h = tb_hash(d, k), mask = d->nslots - 1, i = h & mask;
    long long tag = TB_DTAG(h);
    while (d->slots[i] != 0) {
        long long sl = d->slots[i];
        // метка хеша совпала — только тогда сравниваем сами ключи (текст — дорого)
        if ((sl & ~TB_DAT) == tag) {
            long long at = (sl & TB_DAT) - 1;
            if (tb_keq(d, d->keys->data + at * d->keys->size, k)) return at;
        }
        i = (i + 1) & mask;
    }
    return -1;
}
// память для таблиц dict — там же, где лежат его ключи (временная или обычная)
TB_API void *tb_dalloc(tb_dict *d, void *old, long long bytes) {
    if (old && !(tb_nchunks > 0 && tb_in_temp(old))) tb_gc_free(old);
    void *p = tb_in_cur_temp(d->keys->data) ? tb_temp_alloc(bytes) : tb_gc_alloc_np(bytes);
    if (!p) tb_crash(0, "не хватило памяти");
    return p;
}
TB_API void tb_dreindex(tb_dict *d, long long nslots) {
    d->mode = 0;
    if (d->pos) {
        if (!(tb_nchunks > 0 && tb_in_temp(d->pos))) tb_gc_free(d->pos);
        d->pos = 0; d->npos = 0;
    }
    d->nslots = nslots;
    d->slots = tb_dalloc(d, d->slots, nslots * sizeof(long long));
    memset(d->slots, 0, nslots * sizeof(long long));
    unsigned long long mask = nslots - 1;
    for (long long at = 0; at < d->keys->len; at++) {
        unsigned long long h = tb_hash(d, d->keys->data + at * d->keys->size), i = h & mask;
        while (d->slots[i] != 0) i = (i + 1) & mask;
        d->slots[i] = (at + 1) | TB_DTAG(h);
    }
}
// перейти на таблицу с хешем (ключ ушёл далеко от остальных или его удалили)
TB_API void tb_dto_hash(tb_dict *d) {
    long long n = 8;
    while (n < d->keys->len * 2 + 2) n *= 2;
    tb_dreindex(d, n);
}
// mode 2: таблица pos на ключи base .. base+npos-1
TB_API void tb_dto_pos(tb_dict *d, long long npos) {
    long long *p = tb_dalloc(d, 0, npos * sizeof(long long));
    memset(p, 0, npos * sizeof(long long));
    const long long *ks = (const long long *)d->keys->data;
    for (long long at = 0; at < d->keys->len; at++) p[ks[at] - d->base] = at + 1;
    if (d->pos && !(tb_nchunks > 0 && tb_in_temp(d->pos))) tb_gc_free(d->pos);
    d->pos = p;
    d->npos = npos;
    d->mode = 2;
}
TB_API tb_dict *tb_dict_new(int text, long long vsize) {
    tb_dict *d = tb_mem(sizeof(tb_dict));
    d->text = text;
    d->keys = text ? tb_list_new(sizeof(const char *)) : tb_list_new_np(sizeof(long long));
    d->vals = tb_list_new(vsize);
    d->slots = 0; d->pos = 0; d->npos = 0; d->base = 0;
    if (text) tb_dreindex(d, 8);
    else { d->mode = 1; d->nslots = 0; }
    return d;
}
// .clear(): список снова пустой; свой кусок памяти отдаётся (если он не временный).
// Тексты и obj внутри не удаляются: на них могут смотреть другие переменные.
TB_API void tb_lclear(tb_list *l, int line) {
    if (l->cap < 0) tb_crash(line, ".clear() нельзя, пока по списку идёт each ... by N");
    int temp = tb_nchunks > 0 && tb_in_temp(l->data);
    if (!temp) tb_gc_free(l->data);
    long long cap = l->size == 0 ? 64 : 4;
    long long bytes = l->size == 0 ? 8 : l->size * 4;
    l->data = tb_in_cur_temp(l->data) ? tb_temp_alloc(bytes) : tb_gc_alloc(bytes);
    if (l->data == 0) tb_crash(line, "не хватило памяти");
    memset(l->data, 0, bytes);
    l->len = 0; l->cap = cap;
}
// --llvm: число ключей (из .ll не надо знать устройство tb_dict)
TB_INL long long tb_dlen(tb_dict *d) { return d->keys->len; }
TB_INL tb_list *tb_dkeys_copy(tb_dict *d) { return tb_copy(d->keys); }
TB_INL tb_list *tb_dvals_copy(tb_dict *d) { return tb_copy(d->vals); }
TB_API void tb_dclear(tb_dict *d, int line) {
    tb_lclear(d->keys, line);
    tb_lclear(d->vals, line);
    if (d->pos && !(tb_nchunks > 0 && tb_in_temp(d->pos))) tb_gc_free(d->pos);
    if (d->slots && !(tb_nchunks > 0 && tb_in_temp(d->slots))) tb_gc_free(d->slots);
    d->slots = 0; d->pos = 0; d->npos = 0; d->base = 0;
    if (d->text) tb_dreindex(d, 8);
    else { d->mode = 1; d->nslots = 0; }
}
// новый ключ int k: можно ли остаться без поиска (mode 1 или 2)
TB_API void tb_dplace_int(tb_dict *d, long long k) {
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
TB_API void *tb_dput(tb_dict *d, const void *k, const void *def) {
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
        unsigned long long h = tb_hash(d, k), mask = d->nslots - 1, i = h & mask;
        while (d->slots[i] != 0) i = (i + 1) & mask;
        d->slots[i] = d->keys->len | TB_DTAG(h);
    }
    return v;
}
TB_API void *tb_dget(tb_dict *d, const void *k, int line) {
    long long at = tb_dfind(d, k);
    if (TB_RARE(at < 0)) {
        if (d->text) tb_crash(line, tb_cat("в dict нет ключа: ", *(const char * const *)k));
        tb_crash(line, tb_cat("в dict нет ключа: ", tb_itos(*(const long long *)k)));
    }
    return d->vals->data + at * d->vals->size;
}
// together: шаг 1 — просим память принести место ключа заранее
TB_INL unsigned long long tb_dpre1(tb_dict *d, const void *k) {
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
TB_INL void tb_dpre2(tb_dict *d, unsigned long long i) {
    if (i == ~0ULL || d->mode == 1) return;
    long long s = d->mode == 2 ? d->pos[i] : (d->slots[i] & TB_DAT);
    if (s != 0) {
        if (d->mode == 0) __builtin_prefetch(d->keys->data + (s - 1) * d->keys->size);
        __builtin_prefetch(d->vals->data + (s - 1) * d->vals->size);
    }
}
// элемент списка заранее (prefetch за пределами списка ничего не ломает)
TB_INL void tb_lpre(tb_list *l, long long i) {
    __builtin_prefetch(l->data + i * l->size);
}
// ---------- холодные качества obj: своя память, отдельно от горячих ----------
TB_API void *tb_cold_mem(long long n) {
    if (tb_temp_depth > 0) return tb_mem(n);
    void *r = tb_gc_alloc(n);
    memset(r, 0, n);
    return r;
}
TB_API void *tb_cold_mem_np(long long n) {
    if (tb_temp_depth > 0) return tb_mem(n);
    void *r = tb_gc_alloc_np(n);
    memset(r, 0, n);
    return r;
}
// ---------- obj: память ----------
// Внутри temp и шага цикла — из временной памяти, иначе — из памяти с уборкой.
TB_API void *tb_obj_mem(long long n) {
    if (tb_temp_depth > 0) return tb_mem(n);
    return tb_gc_alloc(n);
}
TB_API void *tb_obj_mem_np(long long n) {
    if (tb_temp_depth > 0) return tb_mem(n);
    return tb_gc_alloc_np(n);
}
// ---------- background: работа в своём потоке ----------
#ifndef TB_BARE
typedef struct { pthread_t th; int done; void *res; } tb_task;
TB_API void tb_task_start(tb_task *t, void *(*run)(void *), void *arg) {
    t->done = 0;
    if (tb_thread_create(&t->th, run, arg) != 0) tb_crash(0, "не получилось запустить background");
}
// a.wait: дождаться потока (один раз) и отдать адрес ответа
TB_API void *tb_wait(tb_task *t) {
    if (!t->done) { pthread_join(t->th, 0); t->done = 1; }
    return t->res;
}
#endif
TB_API int tb_dhas(tb_dict *d, const void *k) { return tb_dfind(d, k) >= 0; }
TB_API void tb_dremove(tb_dict *d, const void *k) {
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
TB_API tb_dict *tb_dict_from(int text, long long vsize, long long n, const void *keys, const void *vals) {
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
TB_API pthread_mutex_t tb_par_lock = PTHREAD_MUTEX_INITIALIZER;
TB_API int tb_cores(void) {
    const char *want = getenv("TB_CORES");   // TB_CORES=1 — проверить на одном ядре
    long n = want != 0 ? atol(want) : sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (n > 256 ? 256 : (int)n);
}
#ifdef TB_BARE
TB_API void tb_par_run(void *(*work)(void *), void *job, long long total) {
    int n = tb_cores();
    if (n > total) n = (int)total;
    if (n < 1) n = 1;
    pthread_t t[256];
    for (int i = 1; i < n; i++)
        if (pthread_create(&t[i], 0, work, job) != 0) tb_crash(0, "не получилось запустить ядро");
    work(job);
    for (int i = 1; i < n; i++) pthread_join(t[i], 0);
}
#else
// Готовые потоки: создаются один раз (при первом on all cores) и ждут работу.
// Раньше каждый цикл на ядрах создавал и закрывал потоки — для коротких циклов это было дольше самой работы.
TB_API pthread_mutex_t tb_pool_m = PTHREAD_MUTEX_INITIALIZER;
TB_API pthread_cond_t tb_pool_go = PTHREAD_COND_INITIALIZER, tb_pool_end = PTHREAD_COND_INITIALIZER;
TB_API int tb_pool_n = 0, tb_pool_want = 0, tb_pool_busy = 0;
TB_API volatile long long tb_pool_gen = 0;
TB_API volatile int tb_pool_left = 0;
TB_API void *(*tb_pool_work)(void *) = 0;
TB_API void *tb_pool_job = 0;
TB_API void *tb_pool_loop(void *arg) {
    int me = (int)(long long)arg;
    // поток пула не считается «работающим», пока ждёт: уборка памяти может идти
    __atomic_sub_fetch(&tb_gthreads, 1, __ATOMIC_RELEASE);
    long long seen = 0;
    for (;;) {
        // немного ждём без сна: циклы на ядрах часто идут один за другим
        for (int spin = 0; spin < 20000 && __atomic_load_n(&tb_pool_gen, __ATOMIC_ACQUIRE) == seen; spin++)
            __builtin_ia32_pause_or_yield();
        pthread_mutex_lock(&tb_pool_m);
        while (tb_pool_gen == seen) pthread_cond_wait(&tb_pool_go, &tb_pool_m);
        seen = tb_pool_gen;
        int mine = me < tb_pool_want;
        void *(*work)(void *) = tb_pool_work;
        void *job = tb_pool_job;
        pthread_mutex_unlock(&tb_pool_m);
        if (!mine) continue;
        work(job);
        __atomic_sub_fetch(&tb_gthreads, 1, __ATOMIC_RELEASE);
        if (__atomic_sub_fetch(&tb_pool_left, 1, __ATOMIC_ACQ_REL) == 0) {
            pthread_mutex_lock(&tb_pool_m);
            pthread_cond_signal(&tb_pool_end);
            pthread_mutex_unlock(&tb_pool_m);
        }
    }
    return 0;
}
TB_API void tb_par_run(void *(*work)(void *), void *job, long long total) {
    int n = tb_cores();
    if (n > total) n = (int)total;
    if (n < 1) n = 1;
    if (n == 1) { work(job); return; }
    // пул уже занят (цикл на ядрах из background или изнутри другого) — по-старому, своими потоками
    if (__atomic_exchange_n(&tb_pool_busy, 1, __ATOMIC_ACQ_REL)) {
        pthread_t t[256];
        for (int i = 1; i < n; i++)
            if (tb_thread_create(&t[i], work, job) != 0) tb_crash(0, "не получилось запустить ядро");
        work(job);
        for (int i = 1; i < n; i++) pthread_join(t[i], 0);
        return;
    }
    while (tb_pool_n < n - 1) {
        pthread_t th;
        if (tb_thread_create(&th, tb_pool_loop, (void *)(long long)tb_pool_n) != 0) tb_crash(0, "не получилось запустить ядро");
        pthread_detach(th);
        tb_pool_n++;
    }
    pthread_mutex_lock(&tb_pool_m);
    tb_pool_work = work; tb_pool_job = job; tb_pool_want = n - 1;
    __atomic_store_n(&tb_pool_left, n - 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&tb_gthreads, n - 1, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&tb_pool_gen, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&tb_pool_go);
    pthread_mutex_unlock(&tb_pool_m);
    work(job);
    if (__atomic_load_n(&tb_pool_left, __ATOMIC_ACQUIRE) != 0) {
        for (int spin = 0; spin < 20000 && __atomic_load_n(&tb_pool_left, __ATOMIC_ACQUIRE) != 0; spin++)
            __builtin_ia32_pause_or_yield();
        pthread_mutex_lock(&tb_pool_m);
        while (__atomic_load_n(&tb_pool_left, __ATOMIC_ACQUIRE) != 0) pthread_cond_wait(&tb_pool_end, &tb_pool_m);
        pthread_mutex_unlock(&tb_pool_m);
    }
    __atomic_store_n(&tb_pool_busy, 0, __ATOMIC_RELEASE);
}
#endif
TB_API long long tb_par_step(long long total) {
    long long s = total / ((long long)tb_cores() * 16);
    return s < 1 ? 1 : s;
}

// ---------- биты, адреса, порты, замки (danger и lock) ----------
TB_INL long long tb_band(long long a, long long b) { return a & b; }
TB_INL long long tb_bor(long long a, long long b) { return a | b; }
TB_INL long long tb_bxor(long long a, long long b) { return a ^ b; }
TB_INL long long tb_bnot(long long a) { return ~a; }
TB_INL long long tb_shl(long long a, long long n) { return (long long)((unsigned long long)a << (n & 63)); }
TB_INL long long tb_shr(long long a, long long n) { return (long long)((unsigned long long)a >> (n & 63)); }
// чтение и запись по адресу: volatile — компилятор C не выкинет и не переставит их
TB_INL long long tb_rd8(long long a) { return *(volatile uint8_t *)(uintptr_t)a; }
TB_INL long long tb_rd16(long long a) { return *(volatile uint16_t *)(uintptr_t)a; }
TB_INL long long tb_rd32(long long a) { return *(volatile uint32_t *)(uintptr_t)a; }
TB_INL long long tb_rd64(long long a) { return (long long)*(volatile uint64_t *)(uintptr_t)a; }
TB_INL void tb_wr8(long long a, long long v) { *(volatile uint8_t *)(uintptr_t)a = (uint8_t)v; }
TB_INL void tb_wr16(long long a, long long v) { *(volatile uint16_t *)(uintptr_t)a = (uint16_t)v; }
TB_INL void tb_wr32(long long a, long long v) { *(volatile uint32_t *)(uintptr_t)a = (uint32_t)v; }
TB_INL void tb_wr64(long long a, long long v) { *(volatile uint64_t *)(uintptr_t)a = (uint64_t)v; }
TB_INL long long tb_addr_of(tb_list *l) { return (long long)(uintptr_t)l->data; }
#ifdef TB_BARE
TB_INL long long tb_pin8(long long p) { return tb_inb((unsigned short)p); }
TB_INL long long tb_pin16(long long p) { return tb_inw((unsigned short)p); }
TB_INL long long tb_pin32(long long p) { return tb_inl((unsigned short)p); }
TB_INL void tb_pout8(long long p, long long v) { tb_outb((unsigned short)p, (unsigned char)v); }
TB_INL void tb_pout16(long long p, long long v) { tb_outw((unsigned short)p, (unsigned short)v); }
TB_INL void tb_pout32(long long p, long long v) { tb_outl((unsigned short)p, (unsigned int)v); }
// замок в ядре: одна команда xchg + прерывания выключены, пока замок у нас
// (так прерывание не застрянет на замке, который держит прерванная задача)
TB_INL unsigned long tb_lock_take(int *l) {
    uint32_t f = tb_irq_save();
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) { tb_irq_restore(f); tb_yield(); f = tb_irq_save(); }
    return f;
}
TB_INL void tb_lock_give(int *l, unsigned long f) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); tb_irq_restore((uint32_t)f); }
#else
// замок в программе: атомарный обмен; занято — сначала коротко ждём, потом уступаем ядро
TB_INL unsigned long tb_lock_take(int *l) {
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) {
        for (int i = 0; __atomic_load_n(l, __ATOMIC_RELAXED); i++) {
            if (i >= 64) sched_yield();
#if defined(__x86_64__) || defined(__i386__)
            else __builtin_ia32_pause();
#endif
        }
    }
    return 0;
}
TB_INL void tb_lock_give(int *l, unsigned long f) { (void)f; __atomic_store_n(l, 0, __ATOMIC_RELEASE); }
#endif

// ---------- канал: очередь между потоками (channel int / text ...) ----------
// send кладёт в конец (очередь растёт, отправитель не ждёт), receive берёт из начала
// и ждёт, пока что-то придёт. close: больше не пишут; each по каналу кончается,
// когда он закрыт и пуст. Текст копируется: отправитель может удалить свою память.
typedef struct {
#ifdef TB_BARE
    int lk;
#else
    pthread_mutex_t m; pthread_cond_t cv;
#endif
    char *buf; long long cap, head, len, size; int text, closed;
} tb_chan;
TB_API tb_chan *tb_chan_new(long long size, int text) {
    tb_chan *ch = tb_gc_alloc(sizeof(tb_chan));
    if (!ch) tb_crash(0, "не хватило памяти");
    memset(ch, 0, sizeof(tb_chan));
#ifndef TB_BARE
    pthread_mutex_init(&ch->m, 0); pthread_cond_init(&ch->cv, 0);
#endif
    ch->size = size; ch->text = text; ch->cap = 16;
    ch->buf = tb_gc_alloc(ch->cap * size);
    if (!ch->buf) tb_crash(0, "не хватило памяти");
    return ch;
}
#ifdef TB_BARE
#define TB_CH_LOCK(ch) unsigned long tb_chf = tb_lock_take(&(ch)->lk)
#define TB_CH_UNLOCK(ch) tb_lock_give(&(ch)->lk, tb_chf)
#else
#define TB_CH_LOCK(ch) pthread_mutex_lock(&(ch)->m)
#define TB_CH_UNLOCK(ch) pthread_mutex_unlock(&(ch)->m)
#endif
TB_API void tb_chan_send(tb_chan *ch, const void *v, int line) {
    char tmp[16];
    const void *src = v;
    if (ch->text) {
        const char *t = *(const char * const *)v;
        size_t n = strlen(t) + 1;
        char *copy = tb_gc_alloc(n);
        if (!copy) tb_crash(line, "не хватило памяти");
        memcpy(copy, t, n);
        memcpy(tmp, &copy, sizeof copy);
        src = tmp;
    }
    TB_CH_LOCK(ch);
    if (ch->closed) { TB_CH_UNLOCK(ch); tb_crash(line, "канал уже закрыт: в него больше не пишут"); }
    if (ch->len == ch->cap) {
        char *nb = tb_gc_alloc(ch->cap * 2 * ch->size);
        if (!nb) { TB_CH_UNLOCK(ch); tb_crash(line, "не хватило памяти"); }
        for (long long i = 0; i < ch->len; i++)
            memcpy(nb + i * ch->size, ch->buf + ((ch->head + i) % ch->cap) * ch->size, ch->size);
        tb_gc_free(ch->buf); ch->buf = nb; ch->head = 0; ch->cap *= 2;
    }
    memcpy(ch->buf + ((ch->head + ch->len) % ch->cap) * ch->size, src, ch->size);
    ch->len++;
#ifndef TB_BARE
    pthread_cond_signal(&ch->cv);
#endif
    TB_CH_UNLOCK(ch);
}
// взять следующее значение; 0 — канал закрыт и пуст
TB_API int tb_chan_take(tb_chan *ch, void *out) {
#ifdef TB_BARE
    unsigned long tb_chf;
    for (;;) {
        tb_chf = tb_lock_take(&ch->lk);
        if (ch->len > 0 || ch->closed) break;
        tb_lock_give(&ch->lk, tb_chf);
        tb_yield();
    }
#else
    TB_CH_LOCK(ch);
    while (ch->len == 0 && !ch->closed) pthread_cond_wait(&ch->cv, &ch->m);
#endif
    if (ch->len == 0) { TB_CH_UNLOCK(ch); return 0; }
    memcpy(out, ch->buf + ch->head * ch->size, ch->size);
    ch->head = (ch->head + 1) % ch->cap;
    ch->len--;
    TB_CH_UNLOCK(ch);
    return 1;
}
TB_API void tb_chan_close(tb_chan *ch) {
    TB_CH_LOCK(ch);
    ch->closed = 1;
#ifndef TB_BARE
    pthread_cond_broadcast(&ch->cv);
#endif
    TB_CH_UNLOCK(ch);
}
#define TB_CH_RECV(T, ch, line) ({ T tb_cv; if (!tb_chan_take((ch), &tb_cv)) tb_crash((line), "канал закрыт и пуст: receive() нечего взять"); tb_cv; })

// ---------- окно и рисование (только если программа рисует) ----------
#if defined(TB_GFX) && !defined(TB_BARE)
TB_API const unsigned char tb_font[161][16] = {
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x64,0x64,0x64,0x64,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x12,0x16,0x7f,0x24,0x2c,0xfe,0x68,0x48,0x00,0x00},
{0x00,0x00,0x00,0x00,0x10,0x10,0x3c,0x78,0x70,0x3c,0x1e,0x16,0x7e,0x3c,0x10,0x10},
{0x00,0x00,0x00,0x00,0x00,0x70,0xd0,0xd0,0x72,0x18,0x4e,0x0b,0x0b,0x0e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x38,0x64,0x30,0x30,0x7b,0xcf,0xce,0x6e,0x3f,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
{0x00,0x00,0x00,0x0c,0x18,0x18,0x18,0x10,0x30,0x30,0x10,0x18,0x18,0x18,0x0c,0x00},
{0x00,0x00,0x00,0x30,0x10,0x18,0x18,0x18,0x08,0x08,0x18,0x18,0x18,0x10,0x30,0x00},
{0x00,0x00,0x00,0x00,0x00,0x18,0x5a,0x3c,0x3c,0x5a,0x18,0x00,0x00,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0xfe,0xfe,0x18,0x18,0x18,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x3c,0x00,0x00,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x06,0x04,0x0c,0x0c,0x08,0x18,0x10,0x30,0x20,0x60,0x40},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x66,0x66,0x7e,0x7e,0x66,0x66,0x66,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x4c,0x06,0x0e,0x0c,0x18,0x30,0x60,0x7e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x46,0x06,0x3c,0x06,0x06,0x06,0x46,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x0c,0x1c,0x3c,0x6c,0x4c,0x7e,0x0c,0x0c,0x0c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7c,0x60,0x60,0x7c,0x4e,0x06,0x06,0x4e,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x60,0x60,0x7c,0x66,0x66,0x66,0x66,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x06,0x0c,0x0c,0x1c,0x18,0x18,0x30,0x30,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x66,0x66,0x3c,0x66,0x66,0x66,0x66,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x6e,0x66,0x66,0x6e,0x3e,0x06,0x4c,0x38,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x18,0x30},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x1e,0x78,0xe0,0x78,0x1e,0x02,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xfe,0xfe,0x00,0xfe,0xfe,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x70,0x1e,0x06,0x1e,0x70,0x40,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x06,0x06,0x0c,0x18,0x18,0x00,0x18,0x18,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x62,0x5e,0xd2,0xb2,0xb2,0xb2,0xd2,0x5e,0x62,0x1e},
{0x00,0x00,0x00,0x00,0x00,0x18,0x3c,0x3c,0x2c,0x64,0x7e,0x66,0x46,0xc3,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x7c,0x66,0x66,0x66,0x7c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x1c,0x32,0x60,0x60,0x60,0x60,0x60,0x32,0x1c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7c,0x6e,0x66,0x66,0x66,0x66,0x66,0x6e,0x7c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x60,0x60,0x60,0x7e,0x60,0x60,0x60,0x7e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x60,0x60,0x60,0x7e,0x60,0x60,0x60,0x60,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x1c,0x72,0x60,0x60,0x6e,0x66,0x66,0x36,0x3e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x7e,0x66,0x66,0x66,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x4c,0x7c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x66,0x6c,0x7c,0x78,0x78,0x6c,0x6c,0x66,0x67,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x7e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0xe6,0xe6,0xfe,0xfe,0xda,0xda,0xc2,0xc2,0xc2,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x76,0x76,0x5e,0x4e,0x4e,0x4e,0x46,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x7c,0x60,0x60,0x60,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x0e,0x04},
{0x00,0x00,0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x7c,0x6c,0x66,0x67,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x62,0x60,0x70,0x3c,0x0e,0x06,0x46,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0xc6,0x66,0x66,0x66,0x64,0x3c,0x3c,0x3c,0x38,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0xc3,0xc3,0xdb,0xda,0x5a,0x7e,0x6e,0x66,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0xe6,0x66,0x3c,0x3c,0x18,0x3c,0x3c,0x66,0xc6,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0xc7,0x66,0x6e,0x3c,0x38,0x18,0x18,0x18,0x18,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x06,0x0e,0x1c,0x18,0x38,0x70,0x60,0x7e,0x00,0x00},
{0x00,0x00,0x00,0x1c,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x1c,0x00},
{0x00,0x00,0x00,0x00,0x00,0x40,0x60,0x20,0x30,0x10,0x18,0x08,0x0c,0x0c,0x04,0x06},
{0x00,0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x38,0x00},
{0x00,0x00,0x00,0x00,0x00,0x18,0x3c,0x6c,0x46,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0x30,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x46,0x06,0x7e,0x66,0x66,0x7e,0x00,0x00},
{0x00,0x00,0x00,0x60,0x60,0x60,0x60,0x7c,0x66,0x66,0x66,0x66,0x66,0x7c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1c,0x32,0x60,0x60,0x60,0x32,0x1c,0x00,0x00},
{0x00,0x00,0x00,0x06,0x06,0x06,0x06,0x3e,0x6e,0x66,0x66,0x66,0x6e,0x3e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x66,0x66,0x7e,0x60,0x62,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x0e,0x18,0x18,0x18,0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3e,0x6e,0x66,0x66,0x66,0x6e,0x3e,0x06,0x46},
{0x00,0x00,0x00,0x60,0x60,0x60,0x60,0x7c,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00},
{0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x78,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00},
{0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},
{0x00,0x00,0x00,0x60,0x60,0x60,0x60,0x66,0x6c,0x78,0x78,0x6c,0x66,0x66,0x00,0x00},
{0x00,0x00,0x00,0x70,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x1e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xfe,0xda,0xda,0xda,0xda,0xda,0xda,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x66,0x66,0x66,0x66,0x66,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x66,0x7c,0x60,0x60},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3e,0x6e,0x66,0x66,0x66,0x6e,0x3e,0x06,0x06},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3e,0x38,0x30,0x30,0x30,0x30,0x30,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x64,0x70,0x3c,0x0e,0x46,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x7e,0x30,0x30,0x30,0x30,0x18,0x1e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x6e,0x3e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x2c,0x3c,0x3c,0x18,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc3,0xc3,0xda,0x5a,0x7e,0x6e,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x3c,0x3c,0x18,0x3c,0x6c,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x3c,0x3c,0x3c,0x18,0x18,0x38},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7e,0x0e,0x1c,0x18,0x30,0x70,0x7e,0x00,0x00},
{0x00,0x00,0x00,0x0e,0x18,0x18,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0e,0x00},
{0x00,0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},
{0x00,0x00,0x00,0x70,0x18,0x18,0x18,0x18,0x18,0x0e,0x18,0x18,0x18,0x18,0x70,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x70,0x0e,0x00,0x00,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x18,0x3c,0x3c,0x2c,0x64,0x7e,0x66,0x46,0xc3,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x60,0x60,0x7c,0x66,0x66,0x66,0x66,0x7c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x7c,0x66,0x66,0x66,0x7c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0xff,0xc3,0xc3},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x60,0x60,0x60,0x7e,0x60,0x60,0x60,0x7e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0xdb,0x5e,0x7e,0x3c,0x7c,0x7e,0x5e,0xda,0xdb,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x46,0x06,0x3c,0x06,0x06,0x06,0x46,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x46,0x4e,0x4e,0x4e,0x5e,0x76,0x76,0x66,0x66,0x00,0x00},
{0x00,0x00,0x24,0x38,0x00,0x46,0x4e,0x4e,0x4e,0x5e,0x76,0x76,0x66,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x66,0x6c,0x7c,0x78,0x78,0x6c,0x6c,0x66,0x67,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0xc6,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0xe6,0xe6,0xfe,0xfe,0xda,0xda,0xc2,0xc2,0xc2,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x7e,0x66,0x66,0x66,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x7c,0x60,0x60,0x60,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x1c,0x32,0x60,0x60,0x60,0x60,0x60,0x32,0x1c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00},
{0x00,0x00,0x00,0x00,0x42,0x66,0x66,0x3e,0x3c,0x1c,0x18,0x18,0x70,0x70,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x18,0x3c,0x5e,0xdb,0xdb,0xdb,0x5e,0x3c,0x18,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0xe6,0x66,0x3c,0x3c,0x18,0x3c,0x3c,0x66,0xc6,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0xff,0x03,0x03},
{0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x7e,0x06,0x06,0x06,0x06,0x06,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0xda,0xda,0xda,0xda,0xda,0xda,0xda,0xda,0xfe,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0xda,0xda,0xda,0xda,0xda,0xda,0xda,0xda,0xff,0x03,0x03},
{0x00,0x00,0x00,0x00,0x60,0xf0,0x30,0x30,0x3c,0x3e,0x36,0x36,0x3e,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x02,0xc3,0xc3,0xc3,0xf3,0xfb,0xcb,0xcb,0xfb,0xf3,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x60,0x60,0x60,0x7c,0x66,0x66,0x66,0x66,0x7c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x30,0x7c,0x4c,0x06,0x3e,0x7e,0x06,0x0e,0x7c,0x78,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0xce,0xde,0xdb,0xdb,0xf3,0xd3,0xdb,0xde,0xce,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3e,0x66,0x66,0x66,0x66,0x3e,0x36,0x76,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x46,0x06,0x7e,0x66,0x66,0x7e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x3c,0x60,0x7c,0xe6,0xe6,0x66,0x66,0x66,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7c,0x66,0x66,0x7c,0x66,0x66,0x7c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7e,0x60,0x60,0x60,0x60,0x60,0x60,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7e,0x66,0x66,0x66,0x66,0x66,0xff,0xc3,0xc3},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x66,0x66,0x7e,0x60,0x62,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xda,0x7e,0x7c,0x7e,0x7e,0x5a,0xdb,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7c,0x0e,0x0e,0x3c,0x06,0x06,0x7c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x6e,0x6e,0x7e,0x76,0x76,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x24,0x38,0x00,0x66,0x6e,0x6e,0x7e,0x76,0x76,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x6c,0x78,0x78,0x6c,0x66,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3e,0x36,0x36,0x36,0x36,0x66,0xe6,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xe6,0xe6,0xfe,0xfe,0xda,0xda,0xc2,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x7e,0x66,0x66,0x66,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x66,0x66,0x66,0x66,0x66,0x3c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7e,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7c,0x66,0x66,0x66,0x66,0x66,0x7c,0x60,0x60},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1c,0x32,0x60,0x60,0x60,0x32,0x1c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x3c,0x3c,0x3c,0x18,0x18,0x38},
{0x00,0x00,0x00,0x18,0x18,0x18,0x18,0x3c,0x7e,0x5a,0xda,0x5a,0x7e,0x3c,0x18,0x18},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x3c,0x3c,0x18,0x3c,0x6c,0x66,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x6c,0x6c,0x6c,0x6c,0x6c,0x6c,0x7e,0x02,0x02},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x3e,0x06,0x06,0x06,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xda,0xda,0xda,0xda,0xda,0xda,0xfe,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xda,0xda,0xda,0xda,0xda,0xda,0xff,0x03,0x03},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf0,0x30,0x30,0x3e,0x36,0x36,0x3e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc3,0xc3,0xc3,0xfb,0xdb,0xfb,0xf3,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x60,0x60,0x60,0x7c,0x66,0x66,0x7c,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7c,0x4c,0x0e,0x3e,0x06,0x5c,0x78,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xcc,0xde,0xd2,0xf2,0xd2,0xde,0xcc,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3e,0x66,0x66,0x3e,0x36,0x76,0x66,0x00,0x00},
{0x00,0x00,0x34,0x34,0x00,0x7e,0x60,0x60,0x60,0x7e,0x60,0x60,0x60,0x7e,0x00,0x00},
{0x00,0x00,0x00,0x00,0x34,0x34,0x00,0x3c,0x66,0x66,0x7e,0x60,0x62,0x3c,0x00,0x00}
};
TB_API uint32_t *tb_px = 0;
TB_API int tb_gw = 0, tb_gh = 0, tb_gclosed = 0, tb_mx = 0, tb_my = 0, tb_mdown = 0;
TB_API unsigned char tb_keys[256];
TB_API long long tb_last_frame = 0;
// клавиши: буквы и цифры — свой код ASCII (A..Z, 0..9), остальные:
enum { TB_K_ENTER = 10, TB_K_ESC = 27, TB_K_SPACE = 32, TB_K_BACK = 8, TB_K_TAB = 9,
       TB_K_UP = 17, TB_K_DOWN = 18, TB_K_RIGHT = 19, TB_K_LEFT = 20, TB_K_SHIFT = 21 };
// ---------- рисование на видеокарте (GPU) ----------
// Кадр не рисуется точками на процессоре: каждая фигура записывается как «экземпляр»
// (прямоугольник, круг или буква), а видеокарта рисует их все разом — по тем же правилам
// точек, что и процессор (tb_draw_* ниже). Mac — Metal, Linux и Windows — Vulkan.
// Если видеокарты нет (нет драйвера Vulkan, нет экрана) — рисуем как раньше, на процессоре.
//   TB_GPU=0        — всегда на процессоре
//   TB_GPU_CHECK=1  — рисовать и там и там и сверять каждый кадр точка в точку
//   TB_GPU_INFO=1   — сказать, чем рисуем
typedef struct { int32_t x0, y0, x1, y1, ox, oy; uint32_t c, m; } tb_ginst;   // 32 байта
enum { TB_GI_CAP = 65536 };
TB_API int tb_gpu_on = 0, tb_gpu_check = 0, tb_gpu_first = 1;
TB_API tb_ginst *tb_gi = 0;            // память экземпляров (видна видеокарте)
TB_API int tb_gin = 0;
TB_API uint32_t *tb_gpu_back = 0;      // TB_GPU_CHECK: кадр, прочитанный обратно с видеокарты
TB_API long long tb_gpu_frames = 0, tb_gpu_bad = 0;
TB_API void tb_gpu_flush(int present);   // своя у каждой системы: Metal или Vulkan
TB_API int tb_gpu_want(void) {
    const char *e = getenv("TB_GPU");
    tb_gpu_check = getenv("TB_GPU_CHECK") != 0 && getenv("TB_GPU_CHECK")[0] == '1';
    return !(e && e[0] == '0');
}
TB_API void tb_gpu_say(const char *what) {
    const char *e = getenv("TB_GPU_INFO");
    if ((e && e[0] == '1') || tb_gpu_check) fprintf(stderr, "окно: рисует %s\n", what);
}
TB_API void tb_gpu_report(void) {
    fprintf(stderr, "TB_GPU_CHECK: кадров %lld, разошлось %lld\n", tb_gpu_frames, tb_gpu_bad);
}
// сверить кадр видеокарты с кадром процессора (видны только младшие 24 бита цвета)
TB_API void tb_gpu_compare(void) {
    if (!tb_gpu_back) return;
    tb_gpu_frames++;
    long long n = (long long)tb_gw * tb_gh, bad = 0, first = -1;
    for (long long i = 0; i < n; i++)
        if (((tb_gpu_back[i] ^ tb_px[i]) & 0xFFFFFF) != 0) { if (first < 0) first = i; bad++; }
    if (bad) {
        tb_gpu_bad++;
        if (tb_gpu_bad <= 5)
            fprintf(stderr, "TB_GPU_CHECK: кадр %lld: не совпало точек %lld, первая x=%lld y=%lld: видеокарта %06x, процессор %06x\n",
                tb_gpu_frames, bad, first % tb_gw, first / tb_gw, tb_gpu_back[first] & 0xFFFFFF, tb_px[first] & 0xFFFFFF);
    }
}
TB_INL void tb_gpu_add(long long x0, long long y0, long long x1, long long y1, long long ox, long long oy, uint32_t c, uint32_t m) {
    if (tb_gin == TB_GI_CAP) tb_gpu_flush(0);
    tb_ginst *g = &tb_gi[tb_gin++];
    g->x0 = (int32_t)x0; g->y0 = (int32_t)y0; g->x1 = (int32_t)x1; g->y1 = (int32_t)y1;
    g->ox = (int32_t)ox; g->oy = (int32_t)oy; g->c = c; g->m = m;
}
// прямоугольник: обрезан окном; соседние полоски одного цвета склеиваются (линии, точки по одной)
TB_API void tb_gpu_rect(long long x, long long y, long long w, long long h, long long c) {
    long long x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > tb_gw ? tb_gw : x + w, y1 = y + h > tb_gh ? tb_gh : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    uint32_t cc = (uint32_t)c & 0xFFFFFF;
    if (tb_gin > 0) {
        tb_ginst *g = &tb_gi[tb_gin - 1];
        if (g->m == 0 && g->c == cc) {
            if (g->y0 == y0 && g->y1 == y1) {
                if (g->x1 == x0) { g->x1 = (int32_t)x1; return; }
                if (g->x0 == x1) { g->x0 = (int32_t)x0; return; }
            }
            if (g->x0 == x0 && g->x1 == x1) {
                if (g->y1 == y0) { g->y1 = (int32_t)y1; return; }
                if (g->y0 == y1) { g->y0 = (int32_t)y0; return; }
            }
        }
    }
    tb_gpu_add(x0, y0, x1, y1, 0, 0, cc, 0);
}
TB_API void tb_gpu_circle(long long cx, long long cy, long long r, long long c) {
    if (r < 0 || cx + r < 0 || cy + r < 0 || cx - r >= tb_gw || cy - r >= tb_gh) return;
    if (r <= 16384) {
        long long x0 = cx - r < 0 ? 0 : cx - r, y0 = cy - r < 0 ? 0 : cy - r;
        long long x1 = cx + r + 1 > tb_gw ? tb_gw : cx + r + 1, y1 = cy + r + 1 > tb_gh ? tb_gh : cy + r + 1;
        tb_gpu_add(x0, y0, x1, y1, cx, cy, (uint32_t)c & 0xFFFFFF, (1u << 30) | (uint32_t)r);
        return;
    }
    // огромный круг: по строкам, полосками (то же правило i*i + j*j <= r*r + r)
    long long ja = -r > -cy ? -r : -cy, jb = r < tb_gh - 1 - cy ? r : tb_gh - 1 - cy;
    for (long long j = ja; j <= jb; j++) {
        long long rest = r * r + r - j * j, i = (long long)sqrt((double)rest);
        while (i * i > rest) i--;
        while ((i + 1) * (i + 1) <= rest) i++;
        tb_gpu_rect(cx - i, cy + j, 2 * i + 1, 1, c);
    }
}
TB_API void tb_gpu_glyph(long long x, long long y, int g, long long size, long long c) {
    if (x >= tb_gw || y >= tb_gh || x + 8 * size <= 0 || y + 16 * size <= 0) return;
    if (size >= (1 << 22) || x < -(1LL << 30) || y < -(1LL << 30)) {
        for (int row = 0; row < 16; row++) for (int col = 0; col < 8; col++)
            if (tb_font[g][row] & (0x80 >> col)) tb_gpu_rect(x + col * size, y + row * size, size, size, c);
        return;
    }
    long long x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    long long x1 = x + 8 * size > tb_gw ? tb_gw : x + 8 * size, y1 = y + 16 * size > tb_gh ? tb_gh : y + 16 * size;
    tb_gpu_add(x0, y0, x1, y1, x, y, (uint32_t)c & 0xFFFFFF, (2u << 30) | ((uint32_t)g << 22) | (uint32_t)size);
}
#if !defined(__APPLE__)
#ifndef _WIN32
#include <dlfcn.h>
#endif
// ---------- Vulkan (Linux и Windows) ----------
// Свои описания Vulkan (только то, что нужно): программе не нужны заголовки Vulkan,
// а сама библиотека Vulkan грузится при запуске (dlopen) — нет её, рисуем на процессоре.
#if defined(_WIN32) && !defined(_WIN64)
#define TBVK_CALL __stdcall
#else
#define TBVK_CALL
#endif
typedef int32_t VkResult;
typedef uint32_t VkFlags, VkBool32;
typedef uint64_t VkDeviceSize;
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef struct VkQueue_T *VkQueue;
typedef struct VkCommandBuffer_T *VkCommandBuffer;
typedef uint64_t VkSurfaceKHR, VkSwapchainKHR, VkImage, VkImageView, VkDeviceMemory, VkBuffer, VkRenderPass,
    VkFramebuffer, VkPipeline, VkPipelineLayout, VkShaderModule, VkCommandPool, VkFence, VkSemaphore,
    VkDescriptorSetLayout, VkDescriptorPool, VkDescriptorSet;
typedef void (TBVK_CALL *PFN_vkVoidFunction)(void);
typedef struct { uint32_t width, height; } VkExtent2D;
typedef struct { uint32_t width, height, depth; } VkExtent3D;
typedef struct { int32_t x, y; } VkOffset2D;
typedef struct { int32_t x, y, z; } VkOffset3D;
typedef struct { VkOffset2D offset; VkExtent2D extent; } VkRect2D;
typedef struct { uint32_t sType; const void *pNext; const char *pApplicationName; uint32_t applicationVersion;
    const char *pEngineName; uint32_t engineVersion; uint32_t apiVersion; } VkApplicationInfo;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames; uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames; } VkInstanceCreateInfo;
typedef struct { VkFlags queueFlags; uint32_t queueCount, timestampValidBits; VkExtent3D minImageTransferGranularity; } VkQueueFamilyProperties;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t queueFamilyIndex, queueCount;
    const float *pQueuePriorities; } VkDeviceQueueCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo *pQueueCreateInfos; uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames; const void *pEnabledFeatures; } VkDeviceCreateInfo;
typedef struct { uint32_t minImageCount, maxImageCount; VkExtent2D currentExtent, minImageExtent, maxImageExtent;
    uint32_t maxImageArrayLayers; VkFlags supportedTransforms; uint32_t currentTransform; VkFlags supportedCompositeAlpha;
    VkFlags supportedUsageFlags; } VkSurfaceCapabilitiesKHR;
typedef struct { uint32_t format, colorSpace; } VkSurfaceFormatKHR;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; VkSurfaceKHR surface; uint32_t minImageCount;
    uint32_t imageFormat, imageColorSpace; VkExtent2D imageExtent; uint32_t imageArrayLayers; VkFlags imageUsage;
    uint32_t imageSharingMode, queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices; uint32_t preTransform;
    uint32_t compositeAlpha, presentMode; VkBool32 clipped; VkSwapchainKHR oldSwapchain; } VkSwapchainCreateInfoKHR;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t imageType, format; VkExtent3D extent;
    uint32_t mipLevels, arrayLayers, samples, tiling; VkFlags usage; uint32_t sharingMode, queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices; uint32_t initialLayout; } VkImageCreateInfo;
typedef struct { VkDeviceSize size, alignment; uint32_t memoryTypeBits; } VkMemoryRequirements;
typedef struct { uint32_t memoryTypeCount; struct { VkFlags propertyFlags; uint32_t heapIndex; } memoryTypes[32];
    uint32_t memoryHeapCount; struct { VkDeviceSize size; VkFlags flags; } memoryHeaps[16]; } VkPhysicalDeviceMemoryProperties;
typedef struct { uint32_t sType; const void *pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex; } VkMemoryAllocateInfo;
typedef struct { VkFlags aspectMask; uint32_t baseMipLevel, levelCount, baseArrayLayer, layerCount; } VkImageSubresourceRange;
typedef struct { VkFlags aspectMask; uint32_t mipLevel, baseArrayLayer, layerCount; } VkImageSubresourceLayers;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; VkImage image; uint32_t viewType, format;
    uint32_t components[4]; VkImageSubresourceRange subresourceRange; } VkImageViewCreateInfo;
typedef struct { VkFlags flags; uint32_t format, samples, loadOp, storeOp, stencilLoadOp, stencilStoreOp, initialLayout,
    finalLayout; } VkAttachmentDescription;
typedef struct { uint32_t attachment, layout; } VkAttachmentReference;
typedef struct { VkFlags flags; uint32_t pipelineBindPoint, inputAttachmentCount; const VkAttachmentReference *pInputAttachments;
    uint32_t colorAttachmentCount; const VkAttachmentReference *pColorAttachments, *pResolveAttachments, *pDepthStencilAttachment;
    uint32_t preserveAttachmentCount; const uint32_t *pPreserveAttachments; } VkSubpassDescription;
typedef struct { uint32_t srcSubpass, dstSubpass; VkFlags srcStageMask, dstStageMask, srcAccessMask, dstAccessMask,
    dependencyFlags; } VkSubpassDependency;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t attachmentCount; const VkAttachmentDescription *pAttachments;
    uint32_t subpassCount; const VkSubpassDescription *pSubpasses; uint32_t dependencyCount;
    const VkSubpassDependency *pDependencies; } VkRenderPassCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; VkRenderPass renderPass; uint32_t attachmentCount;
    const VkImageView *pAttachments; uint32_t width, height, layers; } VkFramebufferCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; size_t codeSize; const uint32_t *pCode; } VkShaderModuleCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t stage; VkShaderModule module; const char *pName;
    const void *pSpecializationInfo; } VkPipelineShaderStageCreateInfo;
typedef struct { uint32_t binding, stride, inputRate; } VkVertexInputBindingDescription;
typedef struct { uint32_t location, binding, format, offset; } VkVertexInputAttributeDescription;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t vertexBindingDescriptionCount;
    const VkVertexInputBindingDescription *pVertexBindingDescriptions; uint32_t vertexAttributeDescriptionCount;
    const VkVertexInputAttributeDescription *pVertexAttributeDescriptions; } VkPipelineVertexInputStateCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t topology; VkBool32 primitiveRestartEnable; } VkPipelineInputAssemblyStateCreateInfo;
typedef struct { float x, y, width, height, minDepth, maxDepth; } VkViewport;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t viewportCount; const VkViewport *pViewports;
    uint32_t scissorCount; const VkRect2D *pScissors; } VkPipelineViewportStateCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; VkBool32 depthClampEnable, rasterizerDiscardEnable;
    uint32_t polygonMode; VkFlags cullMode; uint32_t frontFace; VkBool32 depthBiasEnable; float depthBiasConstantFactor,
    depthBiasClamp, depthBiasSlopeFactor, lineWidth; } VkPipelineRasterizationStateCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t rasterizationSamples; VkBool32 sampleShadingEnable;
    float minSampleShading; const uint32_t *pSampleMask; VkBool32 alphaToCoverageEnable, alphaToOneEnable; } VkPipelineMultisampleStateCreateInfo;
typedef struct { VkBool32 blendEnable; uint32_t srcColorBlendFactor, dstColorBlendFactor, colorBlendOp, srcAlphaBlendFactor,
    dstAlphaBlendFactor, alphaBlendOp; VkFlags colorWriteMask; } VkPipelineColorBlendAttachmentState;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; VkBool32 logicOpEnable; uint32_t logicOp, attachmentCount;
    const VkPipelineColorBlendAttachmentState *pAttachments; float blendConstants[4]; } VkPipelineColorBlendStateCreateInfo;
typedef struct { VkFlags stageFlags; uint32_t offset, size; } VkPushConstantRange;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t setLayoutCount; const VkDescriptorSetLayout *pSetLayouts;
    uint32_t pushConstantRangeCount; const VkPushConstantRange *pPushConstantRanges; } VkPipelineLayoutCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t stageCount; const VkPipelineShaderStageCreateInfo *pStages;
    const VkPipelineVertexInputStateCreateInfo *pVertexInputState; const VkPipelineInputAssemblyStateCreateInfo *pInputAssemblyState;
    const void *pTessellationState; const VkPipelineViewportStateCreateInfo *pViewportState;
    const VkPipelineRasterizationStateCreateInfo *pRasterizationState; const VkPipelineMultisampleStateCreateInfo *pMultisampleState;
    const void *pDepthStencilState; const VkPipelineColorBlendStateCreateInfo *pColorBlendState; const void *pDynamicState;
    VkPipelineLayout layout; VkRenderPass renderPass; uint32_t subpass; VkPipeline basePipelineHandle;
    int32_t basePipelineIndex; } VkGraphicsPipelineCreateInfo;
typedef struct { uint32_t binding, descriptorType, descriptorCount; VkFlags stageFlags; const void *pImmutableSamplers; } VkDescriptorSetLayoutBinding;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t bindingCount; const VkDescriptorSetLayoutBinding *pBindings; } VkDescriptorSetLayoutCreateInfo;
typedef struct { uint32_t type, descriptorCount; } VkDescriptorPoolSize;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t maxSets, poolSizeCount; const VkDescriptorPoolSize *pPoolSizes; } VkDescriptorPoolCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkDescriptorPool descriptorPool; uint32_t descriptorSetCount;
    const VkDescriptorSetLayout *pSetLayouts; } VkDescriptorSetAllocateInfo;
typedef struct { VkBuffer buffer; VkDeviceSize offset, range; } VkDescriptorBufferInfo;
typedef struct { uint32_t sType; const void *pNext; VkDescriptorSet dstSet; uint32_t dstBinding, dstArrayElement, descriptorCount,
    descriptorType; const void *pImageInfo; const VkDescriptorBufferInfo *pBufferInfo; const void *pTexelBufferView; } VkWriteDescriptorSet;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; VkDeviceSize size; VkFlags usage; uint32_t sharingMode,
    queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices; } VkBufferCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t queueFamilyIndex; } VkCommandPoolCreateInfo;
typedef struct { uint32_t sType; const void *pNext; VkCommandPool commandPool; uint32_t level, commandBufferCount; } VkCommandBufferAllocateInfo;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; const void *pInheritanceInfo; } VkCommandBufferBeginInfo;
typedef union { float float32[4]; int32_t int32[4]; uint32_t uint32[4]; } VkClearColorValue;
typedef struct { uint32_t sType; const void *pNext; VkRenderPass renderPass; VkFramebuffer framebuffer; VkRect2D renderArea;
    uint32_t clearValueCount; const VkClearColorValue *pClearValues; } VkRenderPassBeginInfo;
typedef struct { uint32_t sType; const void *pNext; VkFlags srcAccessMask, dstAccessMask; uint32_t oldLayout, newLayout,
    srcQueueFamilyIndex, dstQueueFamilyIndex; VkImage image; VkImageSubresourceRange subresourceRange; } VkImageMemoryBarrier;
typedef struct { VkImageSubresourceLayers srcSubresource; VkOffset3D srcOffsets[2]; VkImageSubresourceLayers dstSubresource;
    VkOffset3D dstOffsets[2]; } VkImageBlit;
typedef struct { VkDeviceSize bufferOffset; uint32_t bufferRowLength, bufferImageHeight; VkImageSubresourceLayers imageSubresource;
    VkOffset3D imageOffset; VkExtent3D imageExtent; } VkBufferImageCopy;
typedef struct { uint32_t sType; const void *pNext; uint32_t waitSemaphoreCount; const VkSemaphore *pWaitSemaphores;
    const VkFlags *pWaitDstStageMask; uint32_t commandBufferCount; const VkCommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount; const VkSemaphore *pSignalSemaphores; } VkSubmitInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t waitSemaphoreCount; const VkSemaphore *pWaitSemaphores;
    uint32_t swapchainCount; const VkSwapchainKHR *pSwapchains; const uint32_t *pImageIndices; VkResult *pResults; } VkPresentInfoKHR;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; } VkFenceCreateInfo;
typedef struct { VkFlags linearTilingFeatures, optimalTilingFeatures, bufferFeatures; } VkFormatProperties;
// нужные функции Vulkan: имя, ответ, параметры
#define TBVK_FUNCS(X) \
    X(EnumeratePhysicalDevices, VkResult, (VkInstance, uint32_t *, VkPhysicalDevice *)) \
    X(GetPhysicalDeviceProperties, void, (VkPhysicalDevice, void *)) \
    X(GetPhysicalDeviceQueueFamilyProperties, void, (VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *)) \
    X(GetPhysicalDeviceMemoryProperties, void, (VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *)) \
    X(GetPhysicalDeviceFormatProperties, void, (VkPhysicalDevice, uint32_t, VkFormatProperties *)) \
    X(GetPhysicalDeviceSurfaceSupportKHR, VkResult, (VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *)) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR, VkResult, (VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *)) \
    X(GetPhysicalDeviceSurfaceFormatsKHR, VkResult, (VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *)) \
    X(GetPhysicalDeviceSurfacePresentModesKHR, VkResult, (VkPhysicalDevice, VkSurfaceKHR, uint32_t *, uint32_t *)) \
    X(CreateDevice, VkResult, (VkPhysicalDevice, const VkDeviceCreateInfo *, const void *, VkDevice *)) \
    X(GetDeviceQueue, void, (VkDevice, uint32_t, uint32_t, VkQueue *)) \
    X(CreateSwapchainKHR, VkResult, (VkDevice, const VkSwapchainCreateInfoKHR *, const void *, VkSwapchainKHR *)) \
    X(DestroySwapchainKHR, void, (VkDevice, VkSwapchainKHR, const void *)) \
    X(GetSwapchainImagesKHR, VkResult, (VkDevice, VkSwapchainKHR, uint32_t *, VkImage *)) \
    X(AcquireNextImageKHR, VkResult, (VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *)) \
    X(QueuePresentKHR, VkResult, (VkQueue, const VkPresentInfoKHR *)) \
    X(CreateImage, VkResult, (VkDevice, const VkImageCreateInfo *, const void *, VkImage *)) \
    X(GetImageMemoryRequirements, void, (VkDevice, VkImage, VkMemoryRequirements *)) \
    X(BindImageMemory, VkResult, (VkDevice, VkImage, VkDeviceMemory, VkDeviceSize)) \
    X(CreateImageView, VkResult, (VkDevice, const VkImageViewCreateInfo *, const void *, VkImageView *)) \
    X(CreateBuffer, VkResult, (VkDevice, const VkBufferCreateInfo *, const void *, VkBuffer *)) \
    X(GetBufferMemoryRequirements, void, (VkDevice, VkBuffer, VkMemoryRequirements *)) \
    X(BindBufferMemory, VkResult, (VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize)) \
    X(AllocateMemory, VkResult, (VkDevice, const VkMemoryAllocateInfo *, const void *, VkDeviceMemory *)) \
    X(MapMemory, VkResult, (VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void **)) \
    X(CreateRenderPass, VkResult, (VkDevice, const VkRenderPassCreateInfo *, const void *, VkRenderPass *)) \
    X(CreateFramebuffer, VkResult, (VkDevice, const VkFramebufferCreateInfo *, const void *, VkFramebuffer *)) \
    X(CreateShaderModule, VkResult, (VkDevice, const VkShaderModuleCreateInfo *, const void *, VkShaderModule *)) \
    X(CreateDescriptorSetLayout, VkResult, (VkDevice, const VkDescriptorSetLayoutCreateInfo *, const void *, VkDescriptorSetLayout *)) \
    X(CreateDescriptorPool, VkResult, (VkDevice, const VkDescriptorPoolCreateInfo *, const void *, VkDescriptorPool *)) \
    X(AllocateDescriptorSets, VkResult, (VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *)) \
    X(UpdateDescriptorSets, void, (VkDevice, uint32_t, const VkWriteDescriptorSet *, uint32_t, const void *)) \
    X(CreatePipelineLayout, VkResult, (VkDevice, const VkPipelineLayoutCreateInfo *, const void *, VkPipelineLayout *)) \
    X(CreateGraphicsPipelines, VkResult, (VkDevice, uint64_t, uint32_t, const VkGraphicsPipelineCreateInfo *, const void *, VkPipeline *)) \
    X(CreateCommandPool, VkResult, (VkDevice, const VkCommandPoolCreateInfo *, const void *, VkCommandPool *)) \
    X(AllocateCommandBuffers, VkResult, (VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *)) \
    X(CreateFence, VkResult, (VkDevice, const VkFenceCreateInfo *, const void *, VkFence *)) \
    X(CreateSemaphore, VkResult, (VkDevice, const VkFenceCreateInfo *, const void *, VkSemaphore *)) \
    X(WaitForFences, VkResult, (VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t)) \
    X(ResetFences, VkResult, (VkDevice, uint32_t, const VkFence *)) \
    X(DeviceWaitIdle, VkResult, (VkDevice)) \
    X(BeginCommandBuffer, VkResult, (VkCommandBuffer, const VkCommandBufferBeginInfo *)) \
    X(EndCommandBuffer, VkResult, (VkCommandBuffer)) \
    X(ResetCommandBuffer, VkResult, (VkCommandBuffer, VkFlags)) \
    X(CmdBeginRenderPass, void, (VkCommandBuffer, const VkRenderPassBeginInfo *, uint32_t)) \
    X(CmdEndRenderPass, void, (VkCommandBuffer)) \
    X(CmdBindPipeline, void, (VkCommandBuffer, uint32_t, VkPipeline)) \
    X(CmdBindDescriptorSets, void, (VkCommandBuffer, uint32_t, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet *, uint32_t, const uint32_t *)) \
    X(CmdBindVertexBuffers, void, (VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *, const VkDeviceSize *)) \
    X(CmdPushConstants, void, (VkCommandBuffer, VkPipelineLayout, VkFlags, uint32_t, uint32_t, const void *)) \
    X(CmdDraw, void, (VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t)) \
    X(CmdPipelineBarrier, void, (VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const void *, uint32_t, const void *, uint32_t, const VkImageMemoryBarrier *)) \
    X(CmdBlitImage, void, (VkCommandBuffer, VkImage, uint32_t, VkImage, uint32_t, uint32_t, const VkImageBlit *, uint32_t)) \
    X(CmdCopyImageToBuffer, void, (VkCommandBuffer, VkImage, uint32_t, VkBuffer, uint32_t, const VkBufferImageCopy *)) \
    X(CmdClearColorImage, void, (VkCommandBuffer, VkImage, uint32_t, const VkClearColorValue *, uint32_t, const VkImageSubresourceRange *)) \
    X(QueueSubmit, VkResult, (VkQueue, uint32_t, const VkSubmitInfo *, VkFence))
#define TBVK_DECL(n, r, p) TB_API r (TBVK_CALL *tbvk_##n) p = 0;
TBVK_FUNCS(TBVK_DECL)
TB_API PFN_vkVoidFunction (TBVK_CALL *tbvk_gipa)(VkInstance, const char *) = 0;
TB_API VkResult (TBVK_CALL *tbvk_CreateInstance)(const VkInstanceCreateInfo *, const void *, VkInstance *) = 0;
// шейдеры: compiler/gpu/tb.vert и tb.frag, переведены в SPIR-V (tools/gpu_shaders.py)
// @@SPIRV-BEGIN@@
TB_API const uint32_t tb_spv_vert[579] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000005b, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x000c000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000a, 0x0000001b, 0x0000003b,
    0x00000051, 0x00000053, 0x00000057, 0x00000059, 0x00030003, 0x00000002, 0x000001c2, 0x00040005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000008, 0x00000076, 0x00060005, 0x0000000a,
    0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00030005, 0x0000000f, 0x00000070, 0x00030005,
    0x0000001b, 0x00000071, 0x00060005, 0x00000039, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000,
    0x00060006, 0x00000039, 0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x00000039,
    0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000, 0x00070006, 0x00000039, 0x00000002,
    0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e, 0x00070006, 0x00000039, 0x00000003, 0x435f6c67,
    0x446c6c75, 0x61747369, 0x0065636e, 0x00030005, 0x0000003b, 0x00000000, 0x00030005, 0x0000003d,
    0x00000050, 0x00050006, 0x0000003d, 0x00000000, 0x657a6973, 0x00000000, 0x00030005, 0x0000003f,
    0x00006370, 0x00030005, 0x00000051, 0x00006f76, 0x00030005, 0x00000053, 0x0000006f, 0x00030005,
    0x00000057, 0x006d6376, 0x00030005, 0x00000059, 0x00006d63, 0x00040047, 0x0000000a, 0x0000000b,
    0x0000002a, 0x00040047, 0x0000001b, 0x0000001e, 0x00000000, 0x00030047, 0x00000039, 0x00000002,
    0x00050048, 0x00000039, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x00000039, 0x00000001,
    0x0000000b, 0x00000001, 0x00050048, 0x00000039, 0x00000002, 0x0000000b, 0x00000003, 0x00050048,
    0x00000039, 0x00000003, 0x0000000b, 0x00000004, 0x00030047, 0x0000003d, 0x00000002, 0x00050048,
    0x0000003d, 0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x00000051, 0x0000000e, 0x00040047,
    0x00000051, 0x0000001e, 0x00000000, 0x00040047, 0x00000053, 0x0000001e, 0x00000001, 0x00030047,
    0x00000057, 0x0000000e, 0x00040047, 0x00000057, 0x0000001e, 0x00000001, 0x00040047, 0x00000059,
    0x0000001e, 0x00000002, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015,
    0x00000006, 0x00000020, 0x00000001, 0x00040020, 0x00000007, 0x00000007, 0x00000006, 0x00040020,
    0x00000009, 0x00000001, 0x00000006, 0x0004003b, 0x00000009, 0x0000000a, 0x00000001, 0x00030016,
    0x0000000c, 0x00000020, 0x00040017, 0x0000000d, 0x0000000c, 0x00000002, 0x00040020, 0x0000000e,
    0x00000007, 0x0000000d, 0x0004002b, 0x00000006, 0x00000011, 0x00000001, 0x0004002b, 0x00000006,
    0x00000013, 0x00000000, 0x00020014, 0x00000014, 0x00040017, 0x00000019, 0x00000006, 0x00000004,
    0x00040020, 0x0000001a, 0x00000001, 0x00000019, 0x0004003b, 0x0000001a, 0x0000001b, 0x00000001,
    0x00040015, 0x0000001c, 0x00000020, 0x00000000, 0x0004002b, 0x0000001c, 0x0000001d, 0x00000002,
    0x0004002b, 0x0000001c, 0x00000021, 0x00000000, 0x0004002b, 0x00000006, 0x00000027, 0x00000002,
    0x0004002b, 0x0000001c, 0x0000002d, 0x00000003, 0x0004002b, 0x0000001c, 0x00000031, 0x00000001,
    0x00040017, 0x00000037, 0x0000000c, 0x00000004, 0x0004001c, 0x00000038, 0x0000000c, 0x00000031,
    0x0006001e, 0x00000039, 0x00000037, 0x0000000c, 0x00000038, 0x00000038, 0x00040020, 0x0000003a,
    0x00000003, 0x00000039, 0x0004003b, 0x0000003a, 0x0000003b, 0x00000003, 0x0003001e, 0x0000003d,
    0x0000000d, 0x00040020, 0x0000003e, 0x00000009, 0x0000003d, 0x0004003b, 0x0000003e, 0x0000003f,
    0x00000009, 0x00040020, 0x00000040, 0x00000009, 0x0000000d, 0x0004002b, 0x0000000c, 0x00000044,
    0x40000000, 0x0004002b, 0x0000000c, 0x00000046, 0x3f800000, 0x0004002b, 0x0000000c, 0x00000049,
    0x00000000, 0x00040020, 0x0000004d, 0x00000003, 0x00000037, 0x00040017, 0x0000004f, 0x00000006,
    0x00000002, 0x00040020, 0x00000050, 0x00000003, 0x0000004f, 0x0004003b, 0x00000050, 0x00000051,
    0x00000003, 0x00040020, 0x00000052, 0x00000001, 0x0000004f, 0x0004003b, 0x00000052, 0x00000053,
    0x00000001, 0x00040017, 0x00000055, 0x0000001c, 0x00000002, 0x00040020, 0x00000056, 0x00000003,
    0x00000055, 0x0004003b, 0x00000056, 0x00000057, 0x00000003, 0x00040020, 0x00000058, 0x00000001,
    0x00000055, 0x0004003b, 0x00000058, 0x00000059, 0x00000001, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000007, 0x00000008, 0x00000007,
    0x0004003b, 0x0000000e, 0x0000000f, 0x00000007, 0x0004003b, 0x00000007, 0x00000016, 0x00000007,
    0x0004003b, 0x00000007, 0x0000002a, 0x00000007, 0x0004003d, 0x00000006, 0x0000000b, 0x0000000a,
    0x0003003e, 0x00000008, 0x0000000b, 0x0004003d, 0x00000006, 0x00000010, 0x00000008, 0x000500c7,
    0x00000006, 0x00000012, 0x00000010, 0x00000011, 0x000500ab, 0x00000014, 0x00000015, 0x00000012,
    0x00000013, 0x000300f7, 0x00000018, 0x00000000, 0x000400fa, 0x00000015, 0x00000017, 0x00000020,
    0x000200f8, 0x00000017, 0x00050041, 0x00000009, 0x0000001e, 0x0000001b, 0x0000001d, 0x0004003d,
    0x00000006, 0x0000001f, 0x0000001e, 0x0003003e, 0x00000016, 0x0000001f, 0x000200f9, 0x00000018,
    0x000200f8, 0x00000020, 0x00050041, 0x00000009, 0x00000022, 0x0000001b, 0x00000021, 0x0004003d,
    0x00000006, 0x00000023, 0x00000022, 0x0003003e, 0x00000016, 0x00000023, 0x000200f9, 0x00000018,
    0x000200f8, 0x00000018, 0x0004003d, 0x00000006, 0x00000024, 0x00000016, 0x0004006f, 0x0000000c,
    0x00000025, 0x00000024, 0x0004003d, 0x00000006, 0x00000026, 0x00000008, 0x000500c7, 0x00000006,
    0x00000028, 0x00000026, 0x00000027, 0x000500ab, 0x00000014, 0x00000029, 0x00000028, 0x00000013,
    0x000300f7, 0x0000002c, 0x00000000, 0x000400fa, 0x00000029, 0x0000002b, 0x00000030, 0x000200f8,
    0x0000002b, 0x00050041, 0x00000009, 0x0000002e, 0x0000001b, 0x0000002d, 0x0004003d, 0x00000006,
    0x0000002f, 0x0000002e, 0x0003003e, 0x0000002a, 0x0000002f, 0x000200f9, 0x0000002c, 0x000200f8,
    0x00000030, 0x00050041, 0x00000009, 0x00000032, 0x0000001b, 0x00000031, 0x0004003d, 0x00000006,
    0x00000033, 0x00000032, 0x0003003e, 0x0000002a, 0x00000033, 0x000200f9, 0x0000002c, 0x000200f8,
    0x0000002c, 0x0004003d, 0x00000006, 0x00000034, 0x0000002a, 0x0004006f, 0x0000000c, 0x00000035,
    0x00000034, 0x00050050, 0x0000000d, 0x00000036, 0x00000025, 0x00000035, 0x0003003e, 0x0000000f,
    0x00000036, 0x0004003d, 0x0000000d, 0x0000003c, 0x0000000f, 0x00050041, 0x00000040, 0x00000041,
    0x0000003f, 0x00000013, 0x0004003d, 0x0000000d, 0x00000042, 0x00000041, 0x00050088, 0x0000000d,
    0x00000043, 0x0000003c, 0x00000042, 0x0005008e, 0x0000000d, 0x00000045, 0x00000043, 0x00000044,
    0x00050050, 0x0000000d, 0x00000047, 0x00000046, 0x00000046, 0x00050083, 0x0000000d, 0x00000048,
    0x00000045, 0x00000047, 0x00050051, 0x0000000c, 0x0000004a, 0x00000048, 0x00000000, 0x00050051,
    0x0000000c, 0x0000004b, 0x00000048, 0x00000001, 0x00070050, 0x00000037, 0x0000004c, 0x0000004a,
    0x0000004b, 0x00000049, 0x00000046, 0x00050041, 0x0000004d, 0x0000004e, 0x0000003b, 0x00000013,
    0x0003003e, 0x0000004e, 0x0000004c, 0x0004003d, 0x0000004f, 0x00000054, 0x00000053, 0x0003003e,
    0x00000051, 0x00000054, 0x0004003d, 0x00000055, 0x0000005a, 0x00000059, 0x0003003e, 0x00000057,
    0x0000005a, 0x000100fd, 0x00010038,
};
TB_API const uint32_t tb_spv_frag[878] = {
    0x07230203, 0x00010000, 0x0008000b, 0x000000a2, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0009000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000017, 0x00000030,
    0x00000090, 0x00030010, 0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000009, 0x00000070, 0x00060005, 0x0000000d,
    0x465f6c67, 0x43676172, 0x64726f6f, 0x00000000, 0x00030005, 0x00000014, 0x0000006d, 0x00030005,
    0x00000017, 0x006d6376, 0x00030005, 0x0000001c, 0x0000006b, 0x00030005, 0x00000026, 0x00000072,
    0x00030005, 0x0000002b, 0x00000069, 0x00030005, 0x00000030, 0x00006f76, 0x00030005, 0x00000035,
    0x0000006a, 0x00030005, 0x00000051, 0x00000073, 0x00030005, 0x00000055, 0x00007863, 0x00030005,
    0x0000005d, 0x00007972, 0x00030005, 0x00000065, 0x00000067, 0x00030005, 0x0000006b, 0x00000062,
    0x00040005, 0x0000006f, 0x746e6f46, 0x00000000, 0x00040006, 0x0000006f, 0x00000000, 0x00000066,
    0x00040005, 0x00000071, 0x746e6f66, 0x00000000, 0x00030005, 0x0000008c, 0x00000063, 0x00030005,
    0x00000090, 0x006c6f63, 0x00040047, 0x0000000d, 0x0000000b, 0x0000000f, 0x00030047, 0x00000017,
    0x0000000e, 0x00040047, 0x00000017, 0x0000001e, 0x00000001, 0x00030047, 0x00000030, 0x0000000e,
    0x00040047, 0x00000030, 0x0000001e, 0x00000000, 0x00040047, 0x0000006e, 0x00000006, 0x00000010,
    0x00030047, 0x0000006f, 0x00000002, 0x00050048, 0x0000006f, 0x00000000, 0x00000023, 0x00000000,
    0x00040047, 0x00000071, 0x00000021, 0x00000000, 0x00040047, 0x00000071, 0x00000022, 0x00000000,
    0x00040047, 0x00000090, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000001, 0x00040017, 0x00000007, 0x00000006,
    0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00030016, 0x0000000a, 0x00000020,
    0x00040017, 0x0000000b, 0x0000000a, 0x00000004, 0x00040020, 0x0000000c, 0x00000001, 0x0000000b,
    0x0004003b, 0x0000000c, 0x0000000d, 0x00000001, 0x00040017, 0x0000000e, 0x0000000a, 0x00000002,
    0x00040015, 0x00000012, 0x00000020, 0x00000000, 0x00040020, 0x00000013, 0x00000007, 0x00000012,
    0x00040017, 0x00000015, 0x00000012, 0x00000002, 0x00040020, 0x00000016, 0x00000001, 0x00000015,
    0x0004003b, 0x00000016, 0x00000017, 0x00000001, 0x0004002b, 0x00000012, 0x00000018, 0x00000001,
    0x00040020, 0x00000019, 0x00000001, 0x00000012, 0x0004002b, 0x00000006, 0x0000001e, 0x0000001e,
    0x00020014, 0x00000021, 0x00040020, 0x00000025, 0x00000007, 0x00000006, 0x0004002b, 0x00000012,
    0x00000028, 0x003fffff, 0x0004002b, 0x00000012, 0x0000002c, 0x00000000, 0x00040020, 0x0000002f,
    0x00000001, 0x00000007, 0x0004003b, 0x0000002f, 0x00000030, 0x00000001, 0x00040020, 0x00000031,
    0x00000001, 0x00000006, 0x0004002b, 0x00000012, 0x0000004d, 0x00000002, 0x0004002b, 0x00000006,
    0x00000067, 0x00000016, 0x0004002b, 0x00000012, 0x00000069, 0x000000ff, 0x00040017, 0x0000006c,
    0x00000012, 0x00000004, 0x0004002b, 0x00000012, 0x0000006d, 0x000000a1, 0x0004001c, 0x0000006e,
    0x0000006c, 0x0000006d, 0x0003001e, 0x0000006f, 0x0000006e, 0x00040020, 0x00000070, 0x00000002,
    0x0000006f, 0x0004003b, 0x00000070, 0x00000071, 0x00000002, 0x0004002b, 0x00000006, 0x00000072,
    0x00000000, 0x0004002b, 0x00000006, 0x00000075, 0x00000002, 0x00040020, 0x00000077, 0x00000002,
    0x00000012, 0x0004002b, 0x00000006, 0x0000007b, 0x00000003, 0x0004002b, 0x00000012, 0x0000007e,
    0x00000008, 0x0004002b, 0x00000012, 0x00000083, 0x00000080, 0x00040020, 0x0000008f, 0x00000003,
    0x0000000b, 0x0004003b, 0x0000008f, 0x00000090, 0x00000003, 0x0004002b, 0x00000006, 0x00000092,
    0x00000010, 0x0004002b, 0x00000006, 0x00000097, 0x00000008, 0x0004002b, 0x0000000a, 0x0000009e,
    0x437f0000, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003b, 0x00000013, 0x00000014, 0x00000007,
    0x0004003b, 0x00000013, 0x0000001c, 0x00000007, 0x0004003b, 0x00000025, 0x00000026, 0x00000007,
    0x0004003b, 0x00000025, 0x0000002b, 0x00000007, 0x0004003b, 0x00000025, 0x00000035, 0x00000007,
    0x0004003b, 0x00000025, 0x00000051, 0x00000007, 0x0004003b, 0x00000025, 0x00000055, 0x00000007,
    0x0004003b, 0x00000025, 0x0000005d, 0x00000007, 0x0004003b, 0x00000013, 0x00000065, 0x00000007,
    0x0004003b, 0x00000013, 0x0000006b, 0x00000007, 0x0004003b, 0x00000013, 0x0000008c, 0x00000007,
    0x0004003d, 0x0000000b, 0x0000000f, 0x0000000d, 0x0007004f, 0x0000000e, 0x00000010, 0x0000000f,
    0x0000000f, 0x00000000, 0x00000001, 0x0004006e, 0x00000007, 0x00000011, 0x00000010, 0x0003003e,
    0x00000009, 0x00000011, 0x00050041, 0x00000019, 0x0000001a, 0x00000017, 0x00000018, 0x0004003d,
    0x00000012, 0x0000001b, 0x0000001a, 0x0003003e, 0x00000014, 0x0000001b, 0x0004003d, 0x00000012,
    0x0000001d, 0x00000014, 0x000500c2, 0x00000012, 0x0000001f, 0x0000001d, 0x0000001e, 0x0003003e,
    0x0000001c, 0x0000001f, 0x0004003d, 0x00000012, 0x00000020, 0x0000001c, 0x000500aa, 0x00000021,
    0x00000022, 0x00000020, 0x00000018, 0x000300f7, 0x00000024, 0x00000000, 0x000400fa, 0x00000022,
    0x00000023, 0x0000004b, 0x000200f8, 0x00000023, 0x0004003d, 0x00000012, 0x00000027, 0x00000014,
    0x000500c7, 0x00000012, 0x00000029, 0x00000027, 0x00000028, 0x0004007c, 0x00000006, 0x0000002a,
    0x00000029, 0x0003003e, 0x00000026, 0x0000002a, 0x00050041, 0x00000025, 0x0000002d, 0x00000009,
    0x0000002c, 0x0004003d, 0x00000006, 0x0000002e, 0x0000002d, 0x00050041, 0x00000031, 0x00000032,
    0x00000030, 0x0000002c, 0x0004003d, 0x00000006, 0x00000033, 0x00000032, 0x00050082, 0x00000006,
    0x00000034, 0x0000002e, 0x00000033, 0x0003003e, 0x0000002b, 0x00000034, 0x00050041, 0x00000025,
    0x00000036, 0x00000009, 0x00000018, 0x0004003d, 0x00000006, 0x00000037, 0x00000036, 0x00050041,
    0x00000031, 0x00000038, 0x00000030, 0x00000018, 0x0004003d, 0x00000006, 0x00000039, 0x00000038,
    0x00050082, 0x00000006, 0x0000003a, 0x00000037, 0x00000039, 0x0003003e, 0x00000035, 0x0000003a,
    0x0004003d, 0x00000006, 0x0000003b, 0x0000002b, 0x0004003d, 0x00000006, 0x0000003c, 0x0000002b,
    0x00050084, 0x00000006, 0x0000003d, 0x0000003b, 0x0000003c, 0x0004003d, 0x00000006, 0x0000003e,
    0x00000035, 0x0004003d, 0x00000006, 0x0000003f, 0x00000035, 0x00050084, 0x00000006, 0x00000040,
    0x0000003e, 0x0000003f, 0x00050080, 0x00000006, 0x00000041, 0x0000003d, 0x00000040, 0x0004003d,
    0x00000006, 0x00000042, 0x00000026, 0x0004003d, 0x00000006, 0x00000043, 0x00000026, 0x00050084,
    0x00000006, 0x00000044, 0x00000042, 0x00000043, 0x0004003d, 0x00000006, 0x00000045, 0x00000026,
    0x00050080, 0x00000006, 0x00000046, 0x00000044, 0x00000045, 0x000500ad, 0x00000021, 0x00000047,
    0x00000041, 0x00000046, 0x000300f7, 0x00000049, 0x00000000, 0x000400fa, 0x00000047, 0x00000048,
    0x00000049, 0x000200f8, 0x00000048, 0x000100fc, 0x000200f8, 0x00000049, 0x000200f9, 0x00000024,
    0x000200f8, 0x0000004b, 0x0004003d, 0x00000012, 0x0000004c, 0x0000001c, 0x000500aa, 0x00000021,
    0x0000004e, 0x0000004c, 0x0000004d, 0x000300f7, 0x00000050, 0x00000000, 0x000400fa, 0x0000004e,
    0x0000004f, 0x00000050, 0x000200f8, 0x0000004f, 0x0004003d, 0x00000012, 0x00000052, 0x00000014,
    0x000500c7, 0x00000012, 0x00000053, 0x00000052, 0x00000028, 0x0004007c, 0x00000006, 0x00000054,
    0x00000053, 0x0003003e, 0x00000051, 0x00000054, 0x00050041, 0x00000025, 0x00000056, 0x00000009,
    0x0000002c, 0x0004003d, 0x00000006, 0x00000057, 0x00000056, 0x00050041, 0x00000031, 0x00000058,
    0x00000030, 0x0000002c, 0x0004003d, 0x00000006, 0x00000059, 0x00000058, 0x00050082, 0x00000006,
    0x0000005a, 0x00000057, 0x00000059, 0x0004003d, 0x00000006, 0x0000005b, 0x00000051, 0x00050087,
    0x00000006, 0x0000005c, 0x0000005a, 0x0000005b, 0x0003003e, 0x00000055, 0x0000005c, 0x00050041,
    0x00000025, 0x0000005e, 0x00000009, 0x00000018, 0x0004003d, 0x00000006, 0x0000005f, 0x0000005e,
    0x00050041, 0x00000031, 0x00000060, 0x00000030, 0x00000018, 0x0004003d, 0x00000006, 0x00000061,
    0x00000060, 0x00050082, 0x00000006, 0x00000062, 0x0000005f, 0x00000061, 0x0004003d, 0x00000006,
    0x00000063, 0x00000051, 0x00050087, 0x00000006, 0x00000064, 0x00000062, 0x00000063, 0x0003003e,
    0x0000005d, 0x00000064, 0x0004003d, 0x00000012, 0x00000066, 0x00000014, 0x000500c2, 0x00000012,
    0x00000068, 0x00000066, 0x00000067, 0x000500c7, 0x00000012, 0x0000006a, 0x00000068, 0x00000069,
    0x0003003e, 0x00000065, 0x0000006a, 0x0004003d, 0x00000012, 0x00000073, 0x00000065, 0x0004003d,
    0x00000006, 0x00000074, 0x0000005d, 0x000500c3, 0x00000006, 0x00000076, 0x00000074, 0x00000075,
    0x00070041, 0x00000077, 0x00000078, 0x00000071, 0x00000072, 0x00000073, 0x00000076, 0x0004003d,
    0x00000012, 0x00000079, 0x00000078, 0x0004003d, 0x00000006, 0x0000007a, 0x0000005d, 0x000500c7,
    0x00000006, 0x0000007c, 0x0000007a, 0x0000007b, 0x0004007c, 0x00000012, 0x0000007d, 0x0000007c,
    0x00050084, 0x00000012, 0x0000007f, 0x0000007d, 0x0000007e, 0x000500c2, 0x00000012, 0x00000080,
    0x00000079, 0x0000007f, 0x000500c7, 0x00000012, 0x00000081, 0x00000080, 0x00000069, 0x0003003e,
    0x0000006b, 0x00000081, 0x0004003d, 0x00000012, 0x00000082, 0x0000006b, 0x0004003d, 0x00000006,
    0x00000084, 0x00000055, 0x0004007c, 0x00000012, 0x00000085, 0x00000084, 0x000500c2, 0x00000012,
    0x00000086, 0x00000083, 0x00000085, 0x000500c7, 0x00000012, 0x00000087, 0x00000082, 0x00000086,
    0x000500aa, 0x00000021, 0x00000088, 0x00000087, 0x0000002c, 0x000300f7, 0x0000008a, 0x00000000,
    0x000400fa, 0x00000088, 0x00000089, 0x0000008a, 0x000200f8, 0x00000089, 0x000100fc, 0x000200f8,
    0x0000008a, 0x000200f9, 0x00000050, 0x000200f8, 0x00000050, 0x000200f9, 0x00000024, 0x000200f8,
    0x00000024, 0x00050041, 0x00000019, 0x0000008d, 0x00000017, 0x0000002c, 0x0004003d, 0x00000012,
    0x0000008e, 0x0000008d, 0x0003003e, 0x0000008c, 0x0000008e, 0x0004003d, 0x00000012, 0x00000091,
    0x0000008c, 0x000500c2, 0x00000012, 0x00000093, 0x00000091, 0x00000092, 0x000500c7, 0x00000012,
    0x00000094, 0x00000093, 0x00000069, 0x00040070, 0x0000000a, 0x00000095, 0x00000094, 0x0004003d,
    0x00000012, 0x00000096, 0x0000008c, 0x000500c2, 0x00000012, 0x00000098, 0x00000096, 0x00000097,
    0x000500c7, 0x00000012, 0x00000099, 0x00000098, 0x00000069, 0x00040070, 0x0000000a, 0x0000009a,
    0x00000099, 0x0004003d, 0x00000012, 0x0000009b, 0x0000008c, 0x000500c7, 0x00000012, 0x0000009c,
    0x0000009b, 0x00000069, 0x00040070, 0x0000000a, 0x0000009d, 0x0000009c, 0x00070050, 0x0000000b,
    0x0000009f, 0x00000095, 0x0000009a, 0x0000009d, 0x0000009e, 0x00070050, 0x0000000b, 0x000000a0,
    0x0000009e, 0x0000009e, 0x0000009e, 0x0000009e, 0x00050088, 0x0000000b, 0x000000a1, 0x0000009f,
    0x000000a0, 0x0003003e, 0x00000090, 0x000000a1, 0x000100fd, 0x00010038,
};
// @@SPIRV-END@@
TB_API struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
    VkSurfaceKHR surf; VkSwapchainKHR sc; uint32_t sc_n, sc_w, sc_h; VkImage sc_img[8];
    VkPhysicalDeviceMemoryProperties mp;
    VkImage img; VkImageView view; VkRenderPass rp; VkFramebuffer fb;
    VkPipelineLayout pl; VkPipeline pipe; VkDescriptorSet ds;
    VkBuffer ibuf, fbuf, rbuf; void *rmap;
    VkCommandPool pool; VkCommandBuffer cb; VkFence fence; VkSemaphore sem_get, sem_done;
} tb_vk;
TB_API int tb_vk_memtype(uint32_t bits, VkFlags want) {
    for (uint32_t i = 0; i < tb_vk.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (tb_vk.mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
// буфер в памяти, которую видят и процессор, и видеокарта; map — её адрес для процессора
TB_API int tb_vk_buffer(VkDeviceSize size, VkFlags usage, VkBuffer *buf, void **map) {
    VkBufferCreateInfo bi = { 12, 0, 0, size, usage, 0, 0, 0 };
    if (tbvk_CreateBuffer(tb_vk.dev, &bi, 0, buf)) return 0;
    VkMemoryRequirements mr; tbvk_GetBufferMemoryRequirements(tb_vk.dev, *buf, &mr);
    int t = tb_vk_memtype(mr.memoryTypeBits, 2 | 4);   // HOST_VISIBLE | HOST_COHERENT
    if (t < 0) return 0;
    VkMemoryAllocateInfo ai = { 5, 0, mr.size, (uint32_t)t };
    VkDeviceMemory mem;
    if (tbvk_AllocateMemory(tb_vk.dev, &ai, 0, &mem) || tbvk_BindBufferMemory(tb_vk.dev, *buf, mem, 0)) return 0;
    return tbvk_MapMemory(tb_vk.dev, mem, 0, size, 0, map) == 0;
}
// цепочка кадров окна (swapchain): создаётся заново, если окно поменялось
TB_API int tb_vk_swapchain(void) {
    VkSurfaceCapabilitiesKHR cap;
    if (tbvk_GetPhysicalDeviceSurfaceCapabilitiesKHR(tb_vk.pd, tb_vk.surf, &cap)) return 0;
    VkExtent2D ext = cap.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { ext.width = (uint32_t)tb_gw; ext.height = (uint32_t)tb_gh; }
    if (ext.width == 0 || ext.height == 0) return 0;   // окно свёрнуто
    if (!(cap.supportedUsageFlags & 2)) return 0;      // нужно копировать в кадр окна (TRANSFER_DST)
    uint32_t nf = 0; tbvk_GetPhysicalDeviceSurfaceFormatsKHR(tb_vk.pd, tb_vk.surf, &nf, 0);
    VkSurfaceFormatKHR fm[64]; if (nf > 64) nf = 64;
    if (!nf || tbvk_GetPhysicalDeviceSurfaceFormatsKHR(tb_vk.pd, tb_vk.surf, &nf, fm) < 0) return 0;
    VkSurfaceFormatKHR f = fm[0];
    for (uint32_t i = 0; i < nf; i++) if (fm[i].format == 44 || fm[i].format == 37) { f = fm[i]; break; }   // BGRA8 / RGBA8 UNORM
    uint32_t np = 0, pm[16], mode = 2;   // FIFO есть всегда; MAILBOX (1) лучше — без ожидания
    tbvk_GetPhysicalDeviceSurfacePresentModesKHR(tb_vk.pd, tb_vk.surf, &np, 0); if (np > 16) np = 16;
    tbvk_GetPhysicalDeviceSurfacePresentModesKHR(tb_vk.pd, tb_vk.surf, &np, pm);
    for (uint32_t i = 0; i < np; i++) if (pm[i] == 1) mode = 1;
    uint32_t cnt = cap.minImageCount + 1;
    if (cap.maxImageCount && cnt > cap.maxImageCount) cnt = cap.maxImageCount;
    VkFlags alpha = (cap.supportedCompositeAlpha & 1) ? 1 : cap.supportedCompositeAlpha & (~cap.supportedCompositeAlpha + 1);
    VkSwapchainKHR old = tb_vk.sc;
    VkSwapchainCreateInfoKHR ci = { 1000001000, 0, 0, tb_vk.surf, cnt, f.format, f.colorSpace, ext, 1, 2 | 16, 0, 0, 0,
        cap.currentTransform, alpha, mode, 1, old };
    if (tbvk_CreateSwapchainKHR(tb_vk.dev, &ci, 0, &tb_vk.sc)) { tb_vk.sc = 0; return 0; }
    if (old) tbvk_DestroySwapchainKHR(tb_vk.dev, old, 0);
    tb_vk.sc_n = 8;
    if (tbvk_GetSwapchainImagesKHR(tb_vk.dev, tb_vk.sc, &tb_vk.sc_n, tb_vk.sc_img) < 0) return 0;
    tb_vk.sc_w = ext.width; tb_vk.sc_h = ext.height;
    return 1;
}
TB_API void *tb_vk_lib(void) {
#ifdef _WIN32
    return (void *)LoadLibraryA("vulkan-1.dll");
#else
    void *h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return h ? h : dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
}
TB_API void *tb_vk_sym(void *lib, const char *n) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, n);
#else
    return dlsym(lib, n);
#endif
}
// make_surface: своя у каждой системы (X11 или окно Windows)
TB_API int tb_vk_init(const char *surf_ext, int (*make_surface)(void)) {
    void *lib = tb_vk_lib();
    if (!lib) return 0;
    tbvk_gipa = (PFN_vkVoidFunction (TBVK_CALL *)(VkInstance, const char *))tb_vk_sym(lib, "vkGetInstanceProcAddr");
    if (!tbvk_gipa) return 0;
    tbvk_CreateInstance = (VkResult (TBVK_CALL *)(const VkInstanceCreateInfo *, const void *, VkInstance *))tbvk_gipa(0, "vkCreateInstance");
    if (!tbvk_CreateInstance) return 0;
    VkApplicationInfo app = { 0, 0, "Tachbit", 1, "Tachbit", 1, (1u << 22) };   // Vulkan 1.0
    const char *iext[2] = { "VK_KHR_surface", surf_ext };
    VkInstanceCreateInfo ici = { 1, 0, 0, &app, 0, 0, 2, iext };
    if (tbvk_CreateInstance(&ici, 0, &tb_vk.inst)) return 0;
#define TBVK_LOAD(n, r, p) tbvk_##n = (r (TBVK_CALL *) p)tbvk_gipa(tb_vk.inst, "vk" #n); if (!tbvk_##n) return 0;
    TBVK_FUNCS(TBVK_LOAD)
    if (!make_surface()) return 0;
    // видеокарта: настоящая лучше программной (lavapipe), если есть обе
    uint32_t n = 0; tbvk_EnumeratePhysicalDevices(tb_vk.inst, &n, 0);
    VkPhysicalDevice pds[16]; if (n > 16) n = 16;
    if (!n || tbvk_EnumeratePhysicalDevices(tb_vk.inst, &n, pds) < 0) return 0;
    int best = -1, best_score = -1;
    static uint64_t props[1024];   // VkPhysicalDeviceProperties: вид карты по смещению 16, имя по 20
    for (uint32_t i = 0; i < n; i++) {
        uint32_t nq = 0; tbvk_GetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, 0);
        VkQueueFamilyProperties qp[32]; if (nq > 32) nq = 32;
        tbvk_GetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
        for (uint32_t j = 0; j < nq; j++) {
            VkBool32 ok = 0;
            if (!(qp[j].queueFlags & 1) || tbvk_GetPhysicalDeviceSurfaceSupportKHR(pds[i], j, tb_vk.surf, &ok) || !ok) continue;
            tbvk_GetPhysicalDeviceProperties(pds[i], props);
            uint32_t type = ((uint32_t *)props)[4];
            int score = type == 2 ? 4 : type == 1 ? 3 : type == 3 ? 2 : 1;   // своя карта, встроенная, виртуальная, процессор
            if (score > best_score) { best_score = score; best = (int)i; tb_vk.qf = j; }
            break;
        }
    }
    if (best < 0) return 0;
    tb_vk.pd = pds[best];
    tbvk_GetPhysicalDeviceProperties(tb_vk.pd, props);
    char what[300]; snprintf(what, sizeof what, "видеокарта (Vulkan, %s)", (const char *)props + 20);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { 2, 0, 0, tb_vk.qf, 1, &prio };
    const char *dext[1] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo dci = { 3, 0, 0, 1, &qci, 0, 0, 1, dext, 0 };
    if (tbvk_CreateDevice(tb_vk.pd, &dci, 0, &tb_vk.dev)) return 0;
    tbvk_GetDeviceQueue(tb_vk.dev, tb_vk.qf, 0, &tb_vk.q);
    tbvk_GetPhysicalDeviceMemoryProperties(tb_vk.pd, &tb_vk.mp);
    VkFormatProperties fp; tbvk_GetPhysicalDeviceFormatProperties(tb_vk.pd, 44, &fp);
    if ((fp.optimalTilingFeatures & (0x80 | 0x400)) != (0x80 | 0x400)) return 0;   // COLOR_ATTACHMENT, BLIT_SRC
    if (!tb_vk_swapchain()) return 0;
    // свой кадр на видеокарте (B8G8R8A8: как tb_px в памяти): живёт между кадрами, как tb_px
    VkImageCreateInfo ii = { 14, 0, 0, 1, 44, { (uint32_t)tb_gw, (uint32_t)tb_gh, 1 }, 1, 1, 1, 0, 16 | 2 | 1, 0, 0, 0, 0 };
    if (tbvk_CreateImage(tb_vk.dev, &ii, 0, &tb_vk.img)) return 0;
    VkMemoryRequirements mr; tbvk_GetImageMemoryRequirements(tb_vk.dev, tb_vk.img, &mr);
    int mt = tb_vk_memtype(mr.memoryTypeBits, 1);
    if (mt < 0) mt = tb_vk_memtype(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai = { 5, 0, mr.size, (uint32_t)mt };
    VkDeviceMemory imem;
    if (mt < 0 || tbvk_AllocateMemory(tb_vk.dev, &mai, 0, &imem) || tbvk_BindImageMemory(tb_vk.dev, tb_vk.img, imem, 0)) return 0;
    VkImageViewCreateInfo vci = { 15, 0, 0, tb_vk.img, 1, 44, { 0, 0, 0, 0 }, { 1, 0, 1, 0, 1 } };
    if (tbvk_CreateImageView(tb_vk.dev, &vci, 0, &tb_vk.view)) return 0;
    // проход рисования: старый кадр сохраняется (LOAD), рисуем поверх — как на процессоре
    VkAttachmentDescription ad = { 0, 44, 1, 0, 0, 2, 1, 2, 2 };   // LOAD, STORE, COLOR_ATTACHMENT_OPTIMAL
    VkAttachmentReference ar = { 0, 2 };
    VkSubpassDescription sd = { 0, 0, 0, 0, 1, &ar, 0, 0, 0, 0 };
    VkSubpassDependency dep = { 0xFFFFFFFFu, 0, 0x400 | 0x1000, 0x400, 0x100 | 0x800 | 0x1000, 0x80 | 0x100, 0 };
    VkRenderPassCreateInfo rci = { 38, 0, 0, 1, &ad, 1, &sd, 1, &dep };
    if (tbvk_CreateRenderPass(tb_vk.dev, &rci, 0, &tb_vk.rp)) return 0;
    VkFramebufferCreateInfo fci = { 37, 0, 0, tb_vk.rp, 1, &tb_vk.view, (uint32_t)tb_gw, (uint32_t)tb_gh, 1 };
    if (tbvk_CreateFramebuffer(tb_vk.dev, &fci, 0, &tb_vk.fb)) return 0;
    // шрифт — для шейдера букв (uniform buffer, 161 буква по 16 байт)
    void *fmap;
    if (!tb_vk_buffer(sizeof tb_font, 0x10, &tb_vk.fbuf, &fmap)) return 0;
    memcpy(fmap, tb_font, sizeof tb_font);
    VkDescriptorSetLayoutBinding lb = { 0, 6, 1, 0x10, 0 };   // UNIFORM_BUFFER, FRAGMENT
    VkDescriptorSetLayoutCreateInfo lci = { 32, 0, 0, 1, &lb };
    VkDescriptorSetLayout dsl;
    if (tbvk_CreateDescriptorSetLayout(tb_vk.dev, &lci, 0, &dsl)) return 0;
    VkDescriptorPoolSize ps = { 6, 1 };
    VkDescriptorPoolCreateInfo pci = { 33, 0, 0, 1, 1, &ps };
    VkDescriptorPool dp;
    if (tbvk_CreateDescriptorPool(tb_vk.dev, &pci, 0, &dp)) return 0;
    VkDescriptorSetAllocateInfo dai = { 34, 0, dp, 1, &dsl };
    if (tbvk_AllocateDescriptorSets(tb_vk.dev, &dai, &tb_vk.ds)) return 0;
    VkDescriptorBufferInfo dbi = { tb_vk.fbuf, 0, sizeof tb_font };
    VkWriteDescriptorSet wds = { 35, 0, tb_vk.ds, 0, 0, 1, 6, 0, &dbi, 0 };
    tbvk_UpdateDescriptorSets(tb_vk.dev, 1, &wds, 0, 0);
    VkPushConstantRange pcr = { 1, 0, 8 };   // VERTEX: размер окна
    VkPipelineLayoutCreateInfo plci = { 30, 0, 0, 1, &dsl, 1, &pcr };
    if (tbvk_CreatePipelineLayout(tb_vk.dev, &plci, 0, &tb_vk.pl)) return 0;
    VkShaderModuleCreateInfo smv = { 16, 0, 0, sizeof tb_spv_vert, tb_spv_vert }, smf = { 16, 0, 0, sizeof tb_spv_frag, tb_spv_frag };
    VkShaderModule mv, mf;
    if (tbvk_CreateShaderModule(tb_vk.dev, &smv, 0, &mv) || tbvk_CreateShaderModule(tb_vk.dev, &smf, 0, &mf)) return 0;
    VkPipelineShaderStageCreateInfo st[2] = { { 18, 0, 0, 1, mv, "main", 0 }, { 18, 0, 0, 0x10, mf, "main", 0 } };
    VkVertexInputBindingDescription vb = { 0, sizeof(tb_ginst), 1 };   // INSTANCE
    VkVertexInputAttributeDescription va[3] = { { 0, 0, 108, 0 }, { 1, 0, 102, 16 }, { 2, 0, 101, 24 } };   // R32G32B32A32_SINT, R32G32_SINT, R32G32_UINT
    VkPipelineVertexInputStateCreateInfo vis = { 19, 0, 0, 1, &vb, 3, va };
    VkPipelineInputAssemblyStateCreateInfo ias = { 20, 0, 0, 4, 0 };   // TRIANGLE_STRIP
    VkViewport vp = { 0, 0, (float)tb_gw, (float)tb_gh, 0, 1 };
    VkRect2D sci = { { 0, 0 }, { (uint32_t)tb_gw, (uint32_t)tb_gh } };
    VkPipelineViewportStateCreateInfo vps = { 22, 0, 0, 1, &vp, 1, &sci };
    VkPipelineRasterizationStateCreateInfo rs = { 23, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { 24, 0, 0, 1, 0, 0, 0, 0, 0 };
    VkPipelineColorBlendAttachmentState cba = { 0, 0, 0, 0, 0, 0, 0, 0xF };
    VkPipelineColorBlendStateCreateInfo cbs = { 26, 0, 0, 0, 0, 1, &cba, { 0, 0, 0, 0 } };
    VkGraphicsPipelineCreateInfo gci = { 28, 0, 0, 2, st, &vis, &ias, 0, &vps, &rs, &ms, 0, &cbs, 0, tb_vk.pl, tb_vk.rp, 0, 0, -1 };
    if (tbvk_CreateGraphicsPipelines(tb_vk.dev, 0, 1, &gci, 0, &tb_vk.pipe)) return 0;
    if (!tb_vk_buffer(sizeof(tb_ginst) * TB_GI_CAP, 0x80, &tb_vk.ibuf, (void **)&tb_gi)) return 0;   // VERTEX_BUFFER
    if (tb_gpu_check) {
        if (!tb_vk_buffer((VkDeviceSize)tb_gw * tb_gh * 4, 2, &tb_vk.rbuf, &tb_vk.rmap)) return 0;   // TRANSFER_DST
        tb_gpu_back = (uint32_t *)tb_vk.rmap;
    }
    VkCommandPoolCreateInfo cpi = { 39, 0, 2, tb_vk.qf };   // RESET_COMMAND_BUFFER
    if (tbvk_CreateCommandPool(tb_vk.dev, &cpi, 0, &tb_vk.pool)) return 0;
    VkCommandBufferAllocateInfo cai = { 40, 0, tb_vk.pool, 0, 1 };
    if (tbvk_AllocateCommandBuffers(tb_vk.dev, &cai, &tb_vk.cb)) return 0;
    VkFenceCreateInfo fi = { 8, 0, 0 }, si = { 9, 0, 0 };
    if (tbvk_CreateFence(tb_vk.dev, &fi, 0, &tb_vk.fence) || tbvk_CreateSemaphore(tb_vk.dev, &si, 0, &tb_vk.sem_get)
        || tbvk_CreateSemaphore(tb_vk.dev, &si, 0, &tb_vk.sem_done)) return 0;
    tb_gpu_say(what);
    return 1;
}
TB_INL VkImageMemoryBarrier tb_vk_bar(VkImage img, VkFlags sa, VkFlags da, uint32_t ol, uint32_t nl) {
    VkImageMemoryBarrier b = { 45, 0, sa, da, ol, nl, 0xFFFFFFFFu, 0xFFFFFFFFu, img, { 1, 0, 1, 0, 1 } };
    return b;
}
// нарисовать накопленные фигуры; present — ещё и показать кадр в окне
TB_API void tb_gpu_flush(int present) {
    if (!tb_gin && !present && !tb_gpu_first) return;
    VkCommandBuffer cb = tb_vk.cb;
    VkCommandBufferBeginInfo bi = { 42, 0, 1, 0 };   // ONE_TIME_SUBMIT
    tbvk_ResetCommandBuffer(cb, 0);
    tbvk_BeginCommandBuffer(cb, &bi);
    if (tb_gpu_first) {   // первый кадр: чёрный, как calloc у tb_px
        tb_gpu_first = 0;
        VkImageMemoryBarrier b = tb_vk_bar(tb_vk.img, 0, 0x1000, 0, 7);
        tbvk_CmdPipelineBarrier(cb, 1, 0x1000, 0, 0, 0, 0, 0, 1, &b);
        VkClearColorValue zero = { { 0, 0, 0, 0 } };
        VkImageSubresourceRange rg = { 1, 0, 1, 0, 1 };
        tbvk_CmdClearColorImage(cb, tb_vk.img, 7, &zero, 1, &rg);
        b = tb_vk_bar(tb_vk.img, 0x1000, 0x80 | 0x100, 7, 2);
        tbvk_CmdPipelineBarrier(cb, 0x1000, 0x400, 0, 0, 0, 0, 0, 1, &b);
    }
    if (tb_gin) {
        VkRenderPassBeginInfo rb = { 43, 0, tb_vk.rp, tb_vk.fb, { { 0, 0 }, { (uint32_t)tb_gw, (uint32_t)tb_gh } }, 0, 0 };
        tbvk_CmdBeginRenderPass(cb, &rb, 0);
        tbvk_CmdBindPipeline(cb, 0, tb_vk.pipe);
        tbvk_CmdBindDescriptorSets(cb, 0, tb_vk.pl, 0, 1, &tb_vk.ds, 0, 0);
        VkDeviceSize off = 0;
        tbvk_CmdBindVertexBuffers(cb, 0, 1, &tb_vk.ibuf, &off);
        float size[2] = { (float)tb_gw, (float)tb_gh };
        tbvk_CmdPushConstants(cb, tb_vk.pl, 1, 0, 8, size);
        tbvk_CmdDraw(cb, 4, (uint32_t)tb_gin, 0, 0);
        tbvk_CmdEndRenderPass(cb);
    }
    uint32_t idx = 0;
    int shown = 0;
    if (present) {
        VkResult r = tb_vk.sc ? tbvk_AcquireNextImageKHR(tb_vk.dev, tb_vk.sc, ~0ULL, tb_vk.sem_get, 0, &idx) : -1000001004;
        if (r == -1000001004 || (!tb_vk.sc && r < 0)) { tb_vk_swapchain(); }   // OUT_OF_DATE: окно поменялось
        else if (r >= 0) shown = 1;
    }
    if (shown || (present && tb_gpu_back)) {
        VkImageMemoryBarrier b = tb_vk_bar(tb_vk.img, 0x100, 0x800, 2, 6);
        tbvk_CmdPipelineBarrier(cb, 0x400, 0x1000, 0, 0, 0, 0, 0, 1, &b);
        if (shown) {
            VkImage si = tb_vk.sc_img[idx];
            b = tb_vk_bar(si, 0, 0x1000, 0, 7);
            tbvk_CmdPipelineBarrier(cb, 0x1000, 0x1000, 0, 0, 0, 0, 0, 1, &b);
            VkImageBlit bl = { { 1, 0, 0, 1 }, { { 0, 0, 0 }, { tb_gw, tb_gh, 1 } }, { 1, 0, 0, 1 },
                { { 0, 0, 0 }, { (int32_t)tb_vk.sc_w, (int32_t)tb_vk.sc_h, 1 } } };
            tbvk_CmdBlitImage(cb, tb_vk.img, 6, si, 7, 1, &bl, 0);   // NEAREST: точки остаются точками
            b = tb_vk_bar(si, 0x1000, 0, 7, 1000001002);              // PRESENT_SRC
            tbvk_CmdPipelineBarrier(cb, 0x1000, 0x2000, 0, 0, 0, 0, 0, 1, &b);
        }
        if (present && tb_gpu_back) {
            VkBufferImageCopy cp = { 0, 0, 0, { 1, 0, 0, 1 }, { 0, 0, 0 }, { (uint32_t)tb_gw, (uint32_t)tb_gh, 1 } };
            tbvk_CmdCopyImageToBuffer(cb, tb_vk.img, 6, tb_vk.rbuf, 1, &cp);
        }
        b = tb_vk_bar(tb_vk.img, 0x800, 0x80 | 0x100, 6, 2);
        tbvk_CmdPipelineBarrier(cb, 0x1000, 0x400, 0, 0, 0, 0, 0, 1, &b);
    }
    tbvk_EndCommandBuffer(cb);
    VkFlags wst = 0x1000;
    VkSubmitInfo si = { 4, 0, shown, &tb_vk.sem_get, &wst, 1, &cb, shown, &tb_vk.sem_done };
    tbvk_QueueSubmit(tb_vk.q, 1, &si, tb_vk.fence);
    if (shown) {
        VkPresentInfoKHR pi = { 1000001001, 0, 1, &tb_vk.sem_done, 1, &tb_vk.sc, &idx, 0 };
        VkResult r = tbvk_QueuePresentKHR(tb_vk.q, &pi);
        if (r == -1000001004 || r == 1000001003) tb_vk.sc_w = 0;   // пересоздать в следующий раз
    }
    // ждём видеокарту: память экземпляров снова свободна для следующих фигур
    tbvk_WaitForFences(tb_vk.dev, 1, &tb_vk.fence, 1, ~0ULL);
    tbvk_ResetFences(tb_vk.dev, 1, &tb_vk.fence);
    if (shown && tb_vk.sc_w == 0) { tbvk_DeviceWaitIdle(tb_vk.dev); tb_vk_swapchain(); }
    tb_gin = 0;
    if (present && tb_gpu_back) tb_gpu_compare();
}
#endif
#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#define TB_MSG(R, o, s) ((R(*)(id, SEL))objc_msgSend)((id)(o), sel_getUid(s))
#define TB_MSG1(R, o, s, A, a) ((R(*)(id, SEL, A))objc_msgSend)((id)(o), sel_getUid(s), a)
#define TB_CLS(n) ((id)objc_getClass(n))
TB_API id tb_nsapp = 0, tb_wnd = 0;
// @@MSL-BEGIN@@
TB_API const char tb_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "// то же, что tb.vert и tb.frag (Vulkan), только для Metal на Mac\n"
    "struct TbInst { int4 q; int2 o; uint2 cm; };\n"
    "struct TbV { float4 pos [[position]]; int2 o [[flat]]; uint2 cm [[flat]]; };\n"
    "vertex TbV tb_vert(uint vid [[vertex_id]], uint iid [[instance_id]],\n"
    "                   const device TbInst *ins [[buffer(0)]], constant float2 &size [[buffer(1)]]) {\n"
    "    TbInst t = ins[iid];\n"
    "    float2 p = float2((vid & 1) ? t.q.z : t.q.x, (vid & 2) ? t.q.w : t.q.y);\n"
    "    TbV out;\n"
    "    out.pos = float4(p.x / size.x * 2.0 - 1.0, 1.0 - p.y / size.y * 2.0, 0.0, 1.0);\n"
    "    out.o = t.o;\n"
    "    out.cm = t.cm;\n"
    "    return out;\n"
    "}\n"
    "fragment float4 tb_frag(TbV v [[stage_in]], constant uint4 *font [[buffer(0)]]) {\n"
    "    int2 p = int2(v.pos.xy);\n"
    "    uint m = v.cm.y, k = m >> 30;\n"
    "    if (k == 1u) {\n"
    "        int r = int(m & 0x3FFFFFu), i = p.x - v.o.x, j = p.y - v.o.y;\n"
    "        if (i * i + j * j > r * r + r) discard_fragment();\n"
    "    } else if (k == 2u) {\n"
    "        int s = int(m & 0x3FFFFFu), cx = (p.x - v.o.x) / s, ry = (p.y - v.o.y) / s;\n"
    "        uint g = (m >> 22) & 255u;\n"
    "        uint b = (font[g][ry >> 2] >> (uint(ry & 3) * 8u)) & 255u;\n"
    "        if ((b & (0x80u >> uint(cx))) == 0u) discard_fragment();\n"
    "    }\n"
    "    uint c = v.cm.x;\n"
    "    return float4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), 255.0) / 255.0;\n"
    "}\n";
// @@MSL-END@@
TB_API const unsigned char tb_mac_keys[128] = {
    [0]='A',[1]='S',[2]='D',[3]='F',[4]='H',[5]='G',[6]='Z',[7]='X',[8]='C',[9]='V',[11]='B',[12]='Q',[13]='W',
    [14]='E',[15]='R',[16]='Y',[17]='T',[18]='1',[19]='2',[20]='3',[21]='4',[22]='6',[23]='5',[25]='9',[26]='7',
    [28]='8',[29]='0',[31]='O',[32]='U',[34]='I',[35]='P',[36]=TB_K_ENTER,[37]='L',[38]='J',[40]='K',[45]='N',
    [46]='M',[48]=TB_K_TAB,[49]=TB_K_SPACE,[51]=TB_K_BACK,[53]=TB_K_ESC,[56]=TB_K_SHIFT,[60]=TB_K_SHIFT,
    [76]=TB_K_ENTER,[123]=TB_K_LEFT,[124]=TB_K_RIGHT,[125]=TB_K_DOWN,[126]=TB_K_UP };
// кадр отдаём слою окна готовой картинкой: масштаб под Retina делает видеокарта,
// цвета в пространстве экрана — без пересчёта каждой точки
// CGBitmapContextCreateImage делал копию при записи: каждая новая точка в кадре
// стоила копии страницы памяти. Теперь три своих буфера по кругу: кадр копируется
// одним memcpy, а картинка смотрит прямо в буфер, без копий внутри системы.
TB_API CGColorSpaceRef tb_cgspace = 0;
TB_API uint32_t *tb_pres[3];
TB_API int tb_pres_i = 0;
TB_API id tb_layer = 0;
TB_API BOOL tb_nswnd_close(id v, SEL s, id w) { (void)v; (void)s; (void)w; tb_gclosed = 1; return YES; }
// ---------- Metal (Mac) ----------
// Metal зовём так же, как Cocoa: через objc_msgSend. Шейдер (tb_msl выше, compiler/gpu/tb.metal)
// Mac переводит сам при запуске. Сама библиотека Metal грузится через dlopen: нет её — процессор.
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *);
typedef struct { unsigned long x, y, z; } tb_mtl3;          // MTLOrigin и MTLSize
typedef struct { double r, g, b, a; } tb_mtl_color;         // MTLClearColor
TB_API struct { id dev, queue, pso, tex, ibuf, fbuf, rbuf, layer; } tb_mtl;
TB_API id tb_nsstr(const char *s) { return TB_MSG1(id, TB_CLS("NSString"), "stringWithUTF8String:", const char *, s); }
TB_API int tb_mtl_fail(const char *what, id err) {
    const char *e = getenv("TB_GPU_INFO");
    if ((e && e[0] == '1') || tb_gpu_check) {
        const char *d = err ? TB_MSG(const char *, TB_MSG(id, err, "localizedDescription"), "UTF8String") : "";
        fprintf(stderr, "Metal: %s %s\n", what, d ? d : "");
    }
    return 0;
}
TB_API int tb_mtl_init(id view) {
    void *lib = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return tb_mtl_fail("нет библиотеки Metal", 0);
    id (*make_dev)(void) = (id (*)(void))dlsym(lib, "MTLCreateSystemDefaultDevice");
    id dev = make_dev ? make_dev() : 0;
    if (!dev || !TB_CLS("CAMetalLayer")) return tb_mtl_fail("нет видеокарты", 0);
    id err = 0;
    id shaders = ((id (*)(id, SEL, id, id, id *))objc_msgSend)(dev, sel_getUid("newLibraryWithSource:options:error:"),
        tb_nsstr(tb_msl), 0, &err);
    if (!shaders) return tb_mtl_fail("шейдер не собрался:", err);
    id vf = TB_MSG1(id, shaders, "newFunctionWithName:", id, tb_nsstr("tb_vert"));
    id ff = TB_MSG1(id, shaders, "newFunctionWithName:", id, tb_nsstr("tb_frag"));
    if (!vf || !ff) return tb_mtl_fail("нет функций шейдера", 0);
    id pd = TB_MSG(id, TB_MSG(id, TB_CLS("MTLRenderPipelineDescriptor"), "alloc"), "init");
    TB_MSG1(void, pd, "setVertexFunction:", id, vf);
    TB_MSG1(void, pd, "setFragmentFunction:", id, ff);
    id ca = TB_MSG1(id, TB_MSG(id, pd, "colorAttachments"), "objectAtIndexedSubscript:", unsigned long, 0);
    TB_MSG1(void, ca, "setPixelFormat:", unsigned long, 80);   // BGRA8Unorm: как tb_px в памяти
    tb_mtl.pso = ((id (*)(id, SEL, id, id *))objc_msgSend)(dev, sel_getUid("newRenderPipelineStateWithDescriptor:error:"), pd, &err);
    if (!tb_mtl.pso) return tb_mtl_fail("не собрался конвейер:", err);
    // свой кадр на видеокарте: живёт между кадрами, как tb_px
    id td = ((id (*)(id, SEL, unsigned long, unsigned long, unsigned long, BOOL))objc_msgSend)(TB_CLS("MTLTextureDescriptor"),
        sel_getUid("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"), 80, (unsigned long)tb_gw, (unsigned long)tb_gh, NO);
    TB_MSG1(void, td, "setUsage:", unsigned long, 1 | 4);      // ShaderRead | RenderTarget
    TB_MSG1(void, td, "setStorageMode:", unsigned long, 2);    // Private: только на видеокарте
    tb_mtl.tex = TB_MSG1(id, dev, "newTextureWithDescriptor:", id, td);
    tb_mtl.ibuf = ((id (*)(id, SEL, unsigned long, unsigned long))objc_msgSend)(dev, sel_getUid("newBufferWithLength:options:"),
        sizeof(tb_ginst) * TB_GI_CAP, 0);
    tb_mtl.fbuf = ((id (*)(id, SEL, const void *, unsigned long, unsigned long))objc_msgSend)(dev,
        sel_getUid("newBufferWithBytes:length:options:"), tb_font, sizeof tb_font, 0);
    if (!tb_mtl.tex || !tb_mtl.ibuf || !tb_mtl.fbuf) return tb_mtl_fail("нет памяти на видеокарте", 0);
    if (tb_gpu_check) {
        tb_mtl.rbuf = ((id (*)(id, SEL, unsigned long, unsigned long))objc_msgSend)(dev, sel_getUid("newBufferWithLength:options:"),
            (unsigned long)tb_gw * tb_gh * 4, 0);
        if (!tb_mtl.rbuf) return tb_mtl_fail("нет памяти на видеокарте", 0);
    }
    tb_mtl.queue = TB_MSG(id, dev, "newCommandQueue");
    // слой окна для Metal: кадр того же размера, Retina растягивает его без сглаживания
    id layer = TB_MSG(id, TB_CLS("CAMetalLayer"), "layer");
    TB_MSG1(void, layer, "setDevice:", id, dev);
    TB_MSG1(void, layer, "setPixelFormat:", unsigned long, 80);
    TB_MSG1(void, layer, "setFramebufferOnly:", BOOL, NO);     // в кадр окна копируем (blit)
    TB_MSG1(void, layer, "setDrawableSize:", CGSize, CGSizeMake(tb_gw, tb_gh));
    TB_MSG1(void, layer, "setMagnificationFilter:", id, tb_nsstr("nearest"));
    TB_MSG1(void, layer, "setColorspace:", CGColorSpaceRef, tb_cgspace);
    TB_MSG1(void, view, "setLayer:", id, layer);
    TB_MSG1(void, view, "setWantsLayer:", BOOL, YES);
    tb_mtl.layer = layer;
    tb_mtl.dev = dev;
    tb_gi = (tb_ginst *)TB_MSG(void *, tb_mtl.ibuf, "contents");
    if (tb_gpu_check) tb_gpu_back = (uint32_t *)TB_MSG(void *, tb_mtl.rbuf, "contents");
    char what[300];
    const char *name = TB_MSG(const char *, TB_MSG(id, dev, "name"), "UTF8String");
    snprintf(what, sizeof what, "видеокарта (Metal, %s)", name ? name : "?");
    tb_gpu_say(what);
    return 1;
}
// нарисовать накопленные фигуры; present — ещё и показать кадр в окне
TB_API void tb_gpu_flush(int present) {
    if (!tb_gin && !present && !tb_gpu_first) return;
    void *pool = objc_autoreleasePoolPush();
    id cb = TB_MSG(id, tb_mtl.queue, "commandBuffer");
    if (tb_gin || tb_gpu_first) {
        id rpd = TB_MSG(id, TB_CLS("MTLRenderPassDescriptor"), "renderPassDescriptor");
        id ca = TB_MSG1(id, TB_MSG(id, rpd, "colorAttachments"), "objectAtIndexedSubscript:", unsigned long, 0);
        TB_MSG1(void, ca, "setTexture:", id, tb_mtl.tex);
        // первый кадр: чёрный, как calloc у tb_px; дальше старый кадр сохраняется (Load)
        TB_MSG1(void, ca, "setLoadAction:", unsigned long, tb_gpu_first ? 2 : 1);
        if (tb_gpu_first) { tb_mtl_color z = { 0, 0, 0, 0 }; TB_MSG1(void, ca, "setClearColor:", tb_mtl_color, z); }
        TB_MSG1(void, ca, "setStoreAction:", unsigned long, 1);
        tb_gpu_first = 0;
        id enc = TB_MSG1(id, cb, "renderCommandEncoderWithDescriptor:", id, rpd);
        if (tb_gin) {
            float size[2] = { (float)tb_gw, (float)tb_gh };
            TB_MSG1(void, enc, "setRenderPipelineState:", id, tb_mtl.pso);
            ((void (*)(id, SEL, id, unsigned long, unsigned long))objc_msgSend)(enc, sel_getUid("setVertexBuffer:offset:atIndex:"), tb_mtl.ibuf, 0, 0);
            ((void (*)(id, SEL, const void *, unsigned long, unsigned long))objc_msgSend)(enc, sel_getUid("setVertexBytes:length:atIndex:"), size, 8, 1);
            ((void (*)(id, SEL, id, unsigned long, unsigned long))objc_msgSend)(enc, sel_getUid("setFragmentBuffer:offset:atIndex:"), tb_mtl.fbuf, 0, 0);
            ((void (*)(id, SEL, unsigned long, unsigned long, unsigned long, unsigned long))objc_msgSend)(enc,
                sel_getUid("drawPrimitives:vertexStart:vertexCount:instanceCount:"), 4, 0, 4, (unsigned long)tb_gin);   // TriangleStrip
        }
        TB_MSG(void, enc, "endEncoding");
    }
    if (present) {
        id drw = TB_MSG(id, tb_mtl.layer, "nextDrawable");
        id dt = drw ? TB_MSG(id, drw, "texture") : 0;
        if (dt && (TB_MSG(unsigned long, dt, "width") != (unsigned long)tb_gw || TB_MSG(unsigned long, dt, "height") != (unsigned long)tb_gh)) dt = 0;
        if (dt || tb_gpu_back) {
            tb_mtl3 o = { 0, 0, 0 }, sz = { (unsigned long)tb_gw, (unsigned long)tb_gh, 1 };
            id bl = TB_MSG(id, cb, "blitCommandEncoder");
            if (dt)
                ((void (*)(id, SEL, id, unsigned long, unsigned long, tb_mtl3, tb_mtl3, id, unsigned long, unsigned long, tb_mtl3))objc_msgSend)(bl,
                    sel_getUid("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:"),
                    tb_mtl.tex, 0, 0, o, sz, dt, 0, 0, o);
            if (tb_gpu_back)
                ((void (*)(id, SEL, id, unsigned long, unsigned long, tb_mtl3, tb_mtl3, id, unsigned long, unsigned long, unsigned long))objc_msgSend)(bl,
                    sel_getUid("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:"),
                    tb_mtl.tex, 0, 0, o, sz, tb_mtl.rbuf, 0, (unsigned long)tb_gw * 4, (unsigned long)tb_gw * tb_gh * 4);
            TB_MSG(void, bl, "endEncoding");
        }
        if (dt) TB_MSG1(void, cb, "presentDrawable:", id, drw);
    }
    TB_MSG(void, cb, "commit");
    // ждём видеокарту: память экземпляров снова свободна для следующих фигур
    TB_MSG(void, cb, "waitUntilCompleted");
    objc_autoreleasePoolPop(pool);
    tb_gin = 0;
    if (present && tb_gpu_back) tb_gpu_compare();
}
TB_API int tb_gfx_open(const char *title) {
    tb_nsapp = TB_MSG(id, TB_CLS("NSApplication"), "sharedApplication");
    TB_MSG1(void, tb_nsapp, "setActivationPolicy:", long, 0);
    tb_wnd = ((id(*)(id, SEL, CGRect, unsigned long, unsigned long, BOOL))objc_msgSend)(
        TB_MSG(id, TB_CLS("NSWindow"), "alloc"), sel_getUid("initWithContentRect:styleMask:backing:defer:"),
        CGRectMake(0, 0, tb_gw, tb_gh), 1 | 2 | 4, 2, NO);
    if (!tb_wnd) return 0;
    Class dc = objc_allocateClassPair((Class)TB_CLS("NSObject"), "TachbitWindowDelegate", 0);
    class_addMethod(dc, sel_getUid("windowShouldClose:"), (IMP)tb_nswnd_close, "c@:@");
    objc_registerClassPair(dc);
    TB_MSG1(void, tb_wnd, "setDelegate:", id, TB_MSG(id, TB_MSG(id, (id)dc, "alloc"), "init"));
    id view = TB_MSG(id, TB_MSG(id, TB_CLS("NSView"), "alloc"), "init");
    TB_MSG1(void, tb_wnd, "setContentView:", id, view);
    CGColorSpaceRef space = CGDisplayCopyColorSpace(CGMainDisplayID());
    if (!space) space = CGColorSpaceCreateDeviceRGB();
    tb_cgspace = space;
    if (tb_gpu_want()) tb_gpu_on = tb_mtl_init(view);
    if (!tb_gpu_on) {
        tb_gpu_say("процессор");
        TB_MSG1(void, view, "setWantsLayer:", BOOL, YES);
        tb_layer = TB_MSG(id, view, "layer");
        id nearest = TB_MSG1(id, TB_CLS("NSString"), "stringWithUTF8String:", const char *, "nearest");
        TB_MSG1(void, tb_layer, "setMagnificationFilter:", id, nearest);
        for (int i = 0; i < 3; i++) {
            tb_pres[i] = malloc((size_t)tb_gw * tb_gh * 4);
            if (!tb_pres[i]) return 0;
        }
    }
    TB_MSG1(void, tb_wnd, "setTitle:", id, TB_MSG1(id, TB_CLS("NSString"), "stringWithUTF8String:", const char *, title));
    TB_MSG1(void, tb_wnd, "setAcceptsMouseMovedEvents:", BOOL, YES);
    TB_MSG1(void, tb_wnd, "makeKeyAndOrderFront:", id, 0);
    TB_MSG(void, tb_wnd, "center");
    TB_MSG(void, tb_nsapp, "finishLaunching");
    TB_MSG1(void, tb_nsapp, "activateIgnoringOtherApps:", BOOL, YES);
    return 1;
}
TB_API void tb_gfx_events(void) {
    // пул: временные объекты Cocoa (события, кадры) освобождаются каждый кадр
    void *pool = objc_autoreleasePoolPush();
    if (tb_gpu_on) tb_gpu_flush(1);
    else {
        size_t bytes = (size_t)tb_gw * tb_gh * 4;
        uint32_t *buf = tb_pres[tb_pres_i];
        tb_pres_i = (tb_pres_i + 1) % 3;
        memcpy(buf, tb_px, bytes);
        CGDataProviderRef prov = CGDataProviderCreateWithData(0, buf, bytes, 0);
        CGImageRef img = CGImageCreate(tb_gw, tb_gh, 8, 32, tb_gw * 4, tb_cgspace,
            kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, prov, 0, false, kCGRenderingIntentDefault);
        TB_MSG1(void, tb_layer, "setContents:", id, (id)img);
        CGImageRelease(img);
        CGDataProviderRelease(prov);
        TB_MSG(void, TB_CLS("CATransaction"), "flush");
    }
    id mode = TB_MSG1(id, TB_CLS("NSString"), "stringWithUTF8String:", const char *, "kCFRunLoopDefaultMode");
    while (1) {
        id ev = ((id(*)(id, SEL, unsigned long long, id, id, BOOL))objc_msgSend)(tb_nsapp,
            sel_getUid("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0ULL, 0, mode, YES);
        if (!ev) break;
        unsigned long t = TB_MSG(unsigned long, ev, "type");
        if (t == 1) tb_mdown = 1;
        else if (t == 2) tb_mdown = 0;
        if (t == 1 || t == 2 || t == 5 || t == 6) {
            CGPoint p = TB_MSG(CGPoint, ev, "locationInWindow");
            tb_mx = (int)p.x; tb_my = (int)(tb_gh - p.y);
        }
        if (t == 10 || t == 11) {
            unsigned short k = TB_MSG(unsigned short, ev, "keyCode");
            if (k < 128 && tb_mac_keys[k]) tb_keys[tb_mac_keys[k]] = t == 10;
            continue;   // клавиши не отдаём окну: иначе Mac пищит
        }
        if (t == 12) {  // Shift
            unsigned long m = TB_MSG(unsigned long, ev, "modifierFlags");
            tb_keys[TB_K_SHIFT] = (m & (1UL << 17)) != 0;
        }
        TB_MSG1(void, tb_nsapp, "sendEvent:", id, ev);
    }
    objc_autoreleasePoolPop(pool);
}
#elif defined(_WIN32)
// окно на Windows: обычное окно Win32, кадр рисует GDI (StretchDIBits), нужна -lgdi32
TB_API HWND tb_hwnd = 0;
TB_API BITMAPINFO tb_bmi;
TB_API int tb_win_key(WPARAM v) {
    if ((v >= 'A' && v <= 'Z') || (v >= '0' && v <= '9')) return (int)v;
    switch (v) {
    case VK_RETURN: return TB_K_ENTER; case VK_ESCAPE: return TB_K_ESC; case VK_SPACE: return TB_K_SPACE;
    case VK_BACK: return TB_K_BACK; case VK_TAB: return TB_K_TAB; case VK_UP: return TB_K_UP;
    case VK_DOWN: return TB_K_DOWN; case VK_LEFT: return TB_K_LEFT; case VK_RIGHT: return TB_K_RIGHT;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return TB_K_SHIFT;
    }
    return 0;
}
TB_API void tb_win_blit(HDC dc) {
    StretchDIBits(dc, 0, 0, tb_gw, tb_gh, 0, 0, tb_gw, tb_gh, tb_px, &tb_bmi, DIB_RGB_COLORS, SRCCOPY);
}
TB_API LRESULT CALLBACK tb_win_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: tb_gclosed = 1; return 0;   // окно закроет сама программа: window_alive() вернёт false
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        int k = tb_win_key(wp);
        if (k) tb_keys[k] = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        if (wp != VK_F4) return 0;   // Alt+F4 пусть закрывает окно
        break;
    }
    case WM_MOUSEMOVE: tb_mx = (short)LOWORD(lp); tb_my = (short)HIWORD(lp); return 0;
    case WM_LBUTTONDOWN: tb_mdown = 1; tb_mx = (short)LOWORD(lp); tb_my = (short)HIWORD(lp); SetCapture(h); return 0;
    case WM_LBUTTONUP: tb_mdown = 0; tb_mx = (short)LOWORD(lp); tb_my = (short)HIWORD(lp); ReleaseCapture(); return 0;
    case WM_KILLFOCUS: memset(tb_keys, 0, sizeof tb_keys); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); if (tb_px && !tb_gpu_on) tb_win_blit(dc); EndPaint(h, &ps); return 0; }
    }
    return DefWindowProcW(h, msg, wp, lp);
}
// окно Windows для Vulkan
TB_API int tb_win_surface(void) {
    struct { uint32_t sType; const void *pNext; VkFlags flags; HINSTANCE inst; HWND hwnd; } ci = { 1000009000, 0, 0, GetModuleHandleW(0), tb_hwnd };
    VkResult (TBVK_CALL *mk)(VkInstance, const void *, const void *, VkSurfaceKHR *) =
        (VkResult (TBVK_CALL *)(VkInstance, const void *, const void *, VkSurfaceKHR *))tbvk_gipa(tb_vk.inst, "vkCreateWin32SurfaceKHR");
    return mk && mk(tb_vk.inst, &ci, 0, &tb_vk.surf) == 0;
}
TB_API int tb_gfx_open(const char *title) {
    HINSTANCE inst = GetModuleHandleW(0);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = tb_win_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.lpszClassName = L"TachbitWindow";
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, tb_gw, tb_gh };
    AdjustWindowRect(&r, style, FALSE);
    wchar_t *wt = tb_wide(title);
    tb_hwnd = CreateWindowExW(0, L"TachbitWindow", wt ? wt : L"Tachbit", style, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, 0, 0, inst, 0);
    free(wt);
    if (!tb_hwnd) return 0;
    memset(&tb_bmi, 0, sizeof tb_bmi);
    tb_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    tb_bmi.bmiHeader.biWidth = tb_gw;
    tb_bmi.bmiHeader.biHeight = -tb_gh;   // строки сверху вниз, как в tb_px
    tb_bmi.bmiHeader.biPlanes = 1;
    tb_bmi.bmiHeader.biBitCount = 32;     // 0x00RRGGBB — ровно формат tb_px
    tb_bmi.bmiHeader.biCompression = BI_RGB;
    ShowWindow(tb_hwnd, SW_SHOW);
    SetForegroundWindow(tb_hwnd);
    if (tb_gpu_want()) tb_gpu_on = tb_vk_init("VK_KHR_win32_surface", tb_win_surface);
    if (!tb_gpu_on) tb_gpu_say("процессор");
    return 1;
}
TB_API void tb_gfx_events(void) {
    if (tb_gpu_on) tb_gpu_flush(1);
    else { HDC dc = GetDC(tb_hwnd); if (dc) { tb_win_blit(dc); ReleaseDC(tb_hwnd, dc); } }
    MSG m;
    while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}
#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
TB_API Display *tb_dpy = 0;
TB_API Window tb_xw;
TB_API GC tb_xgc;
TB_API XImage *tb_ximg = 0;
TB_API Atom tb_wm_del;
// окно X11 для Vulkan
TB_API int tb_x_surface(void) {
    struct { uint32_t sType; const void *pNext; VkFlags flags; Display *dpy; Window window; } ci = { 1000004000, 0, 0, tb_dpy, tb_xw };
    VkResult (TBVK_CALL *mk)(VkInstance, const void *, const void *, VkSurfaceKHR *) =
        (VkResult (TBVK_CALL *)(VkInstance, const void *, const void *, VkSurfaceKHR *))tbvk_gipa(tb_vk.inst, "vkCreateXlibSurfaceKHR");
    return mk && mk(tb_vk.inst, &ci, 0, &tb_vk.surf) == 0;
}
TB_API int tb_gfx_open(const char *title) {
    tb_dpy = XOpenDisplay(0);
    if (!tb_dpy) return 0;
    int scr = DefaultScreen(tb_dpy);
    tb_xw = XCreateSimpleWindow(tb_dpy, RootWindow(tb_dpy, scr), 0, 0, tb_gw, tb_gh, 0, 0, 0);
    tb_xgc = XCreateGC(tb_dpy, tb_xw, 0, 0);
    XSelectInput(tb_dpy, tb_xw, ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    Xutf8SetWMProperties(tb_dpy, tb_xw, title, title, 0, 0, 0, 0, 0);
    tb_wm_del = XInternAtom(tb_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(tb_dpy, tb_xw, &tb_wm_del, 1);
    XkbSetDetectableAutoRepeat(tb_dpy, True, 0);
    XMapWindow(tb_dpy, tb_xw);
    tb_ximg = XCreateImage(tb_dpy, DefaultVisual(tb_dpy, scr), 24, ZPixmap, 0, (char *)tb_px, tb_gw, tb_gh, 32, 0);
    if (tb_gpu_want()) tb_gpu_on = tb_vk_init("VK_KHR_xlib_surface", tb_x_surface);
    if (!tb_gpu_on) tb_gpu_say("процессор");
    return 1;
}
TB_API int tb_x_key(KeySym s) {
    if (s >= XK_a && s <= XK_z) return 'A' + (int)(s - XK_a);
    if (s >= XK_A && s <= XK_Z) return 'A' + (int)(s - XK_A);
    if (s >= XK_0 && s <= XK_9) return '0' + (int)(s - XK_0);
    switch (s) {
    case XK_Return: return TB_K_ENTER; case XK_Escape: return TB_K_ESC; case XK_space: return TB_K_SPACE;
    case XK_BackSpace: return TB_K_BACK; case XK_Tab: return TB_K_TAB; case XK_Up: return TB_K_UP;
    case XK_Down: return TB_K_DOWN; case XK_Left: return TB_K_LEFT; case XK_Right: return TB_K_RIGHT;
    case XK_Shift_L: case XK_Shift_R: return TB_K_SHIFT;
    }
    return 0;
}
TB_API void tb_gfx_events(void) {
    if (tb_gpu_on) tb_gpu_flush(1);
    else XPutImage(tb_dpy, tb_xw, tb_xgc, tb_ximg, 0, 0, 0, 0, tb_gw, tb_gh);
    while (XPending(tb_dpy)) {
        XEvent e; XNextEvent(tb_dpy, &e);
        if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == tb_wm_del) tb_gclosed = 1;
        else if (e.type == KeyPress || e.type == KeyRelease) {
            int k = tb_x_key(XLookupKeysym(&e.xkey, 0));
            if (k) tb_keys[k] = e.type == KeyPress;
        } else if (e.type == ButtonPress || e.type == ButtonRelease) {
            if (e.xbutton.button == 1) tb_mdown = e.type == ButtonPress;
            tb_mx = e.xbutton.x; tb_my = e.xbutton.y;
        } else if (e.type == MotionNotify) { tb_mx = e.xmotion.x; tb_my = e.xmotion.y; }
    }
    XFlush(tb_dpy);
}
#endif
TB_API void tb_window_open(long long w, long long h, const char *title) {
    if (tb_px) { tb_err = "окно уже открыто"; return; }
    if (w < 1 || h < 1 || w > 8192 || h > 8192) { tb_err = "размер окна от 1 до 8192"; return; }
    tb_gw = (int)w; tb_gh = (int)h;
    tb_px = calloc((size_t)tb_gw * tb_gh, 4);
    if (!tb_gfx_open(title)) { free(tb_px); tb_px = 0; tb_err = "не могу открыть окно: нет экрана"; return; }
    if (tb_gpu_on && tb_gpu_check) atexit(tb_gpu_report);
    tb_last_frame = tb_time_ms();
}
// показать кадр, принять клавиши и мышь, держать 60 кадров в секунду; false — окно закрыли
TB_API int tb_window_alive(void) {
    if (!tb_px || tb_gclosed) return 0;
    tb_gfx_events();
    // TB_FPS_FREE=1 — без ограничения 60 кадров (для замеров скорости)
    static int free_fps = -1;
    if (free_fps < 0) free_fps = getenv("TB_FPS_FREE") != 0;
    long long now = tb_time_ms(), wait = 16 - (now - tb_last_frame);
    if (wait > 0 && !free_fps) usleep((useconds_t)wait * 1000);
    tb_last_frame = tb_time_ms();
    return !tb_gclosed;
}
TB_INL void tb_put(long long x, long long y, long long c) {
    if (tb_px && x >= 0 && y >= 0 && x < tb_gw && y < tb_gh) tb_px[y * tb_gw + x] = (uint32_t)c;
}
// на видеокарте фигуры записываются (tb_gpu_*), на процессоре — рисуются сразу в tb_px.
// TB_GPU_CHECK=1: и то и другое, кадры сверяются
#define TB_CPU_DRAW (!tb_gpu_on || tb_gpu_check)
TB_API void tb_clear_window(long long c) {
    if (!tb_px) return;
    if (tb_gpu_on) { tb_gin = 0; tb_gpu_rect(0, 0, tb_gw, tb_gh, c); }   // всё прежнее закрашено
    if (TB_CPU_DRAW) for (long long i = 0; i < (long long)tb_gw * tb_gh; i++) tb_px[i] = (uint32_t)c;
}
TB_API void tb_draw_pixel(long long x, long long y, long long c) {
    if (!tb_px) return;
    if (tb_gpu_on) tb_gpu_rect(x, y, 1, 1, c);
    if (TB_CPU_DRAW) tb_put(x, y, c);
}
TB_API void tb_cpu_rect(long long x, long long y, long long w, long long h, long long c) {
    long long x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > tb_gw ? tb_gw : x + w, y1 = y + h > tb_gh ? tb_gh : y + h;
    for (long long j = y0; j < y1; j++) for (long long i = x0; i < x1; i++) tb_px[j * tb_gw + i] = (uint32_t)c;
}
TB_API void tb_draw_rect(long long x, long long y, long long w, long long h, long long c) {
    if (!tb_px) return;
    if (tb_gpu_on) tb_gpu_rect(x, y, w, h, c);
    if (TB_CPU_DRAW) tb_cpu_rect(x, y, w, h, c);
}
TB_API void tb_draw_circle(long long cx, long long cy, long long r, long long c) {
    if (!tb_px) return;
    if (tb_gpu_on) tb_gpu_circle(cx, cy, r, c);
    if (TB_CPU_DRAW) {   // только строки и столбцы внутри окна: огромный круг не перебирает r*r точек
        long long ja = -r > -cy ? -r : -cy, jb = r < tb_gh - 1 - cy ? r : tb_gh - 1 - cy;
        long long ia = -r > -cx ? -r : -cx, ib = r < tb_gw - 1 - cx ? r : tb_gw - 1 - cx;
        for (long long j = ja; j <= jb; j++) for (long long i = ia; i <= ib; i++)
            if (i * i + j * j <= r * r + r) tb_put(cx + i, cy + j, c);
    }
}
TB_API void tb_draw_line(long long x0, long long y0, long long x1, long long y1, long long c) {
    if (!tb_px) return;
    long long dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    long long sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    int gpu = tb_gpu_on, cpu = TB_CPU_DRAW;
    while (1) {
        if (gpu) tb_gpu_rect(x0, y0, 1, 1, c);   // соседние точки склеятся в полоски
        if (cpu) tb_put(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        long long e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
// текст: буква 8×16, size — во сколько раз крупнее
TB_API void tb_draw_text(long long x, long long y, const char *s, long long size, long long c) {
    if (!tb_px) return;
    if (size < 1) size = 1;
    const unsigned char *p = (const unsigned char *)s;
    long long cx = x;
    while (*p) {
        unsigned int u = *p++;
        if (u >= 0x80) {   // UTF-8: собрать букву из 2–4 байт
            int more = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : u >= 0xC0 ? 1 : 0;
            u &= 0x3F >> more;
            while (more-- > 0 && (*p & 0xC0) == 0x80) u = (u << 6) | (*p++ & 0x3F);
        }
        if (u == 0x2013 || u == 0x2014) u = '-';
        if (u == 0xAB || u == 0xBB || u == 0x201C || u == 0x201D) u = '"';
        if (u == '\n') { cx = x; y += 16 * size; continue; }
        int g = -1;
        if (u >= 32 && u <= 126) g = (int)u - 32;
        else if (u >= 0x410 && u <= 0x44F) g = 95 + (int)(u - 0x410);
        else if (u == 0x401) g = 159;
        else if (u == 0x451) g = 160;
        if (g < 0) g = '?' - 32;
        if (tb_gpu_on && g != 0) tb_gpu_glyph(cx, y, g, size, c);   // 0 — пробел, рисовать нечего
        if (TB_CPU_DRAW)
            for (int row = 0; row < 16; row++) {
                unsigned char bits = tb_font[g][row];
                for (int col = 0; col < 8; col++)
                    if (bits & (0x80 >> col)) tb_cpu_rect(cx + col * size, y + row * size, size, size, c);
            }
        cx += 8 * size;
    }
}
TB_API int tb_key_down(const char *name) {
    static const struct { const char *n; int k; } names[] = {
        {"left", TB_K_LEFT}, {"right", TB_K_RIGHT}, {"up", TB_K_UP}, {"down", TB_K_DOWN}, {"space", TB_K_SPACE},
        {"enter", TB_K_ENTER}, {"escape", TB_K_ESC}, {"backspace", TB_K_BACK}, {"tab", TB_K_TAB}, {"shift", TB_K_SHIFT} };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) if (strcmp(name, names[i].n) == 0) return tb_keys[names[i].k];
    if (name[0] && !name[1]) {
        int ch = (unsigned char)name[0];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        return tb_keys[ch];
    }
    return 0;
}
TB_API long long tb_mouse_x(void) { return tb_mx; }
TB_API long long tb_mouse_y(void) { return tb_my; }
TB_API int tb_mouse_down(void) { return tb_mdown; }
#endif
// ---------- для файла .ll (TB_LL): библиотека видна снаружи через tb_x_* ----------
// Сама библиотека — static: так её лучше встраивать. Эти обёртки — её «дверь» для LLVM.
// Без clang (llvm-link + opt + llc) библиотека заранее лежит в файле .bc, а программа
// целиком в .ll: показ значений, конструкторы и main тоже пишутся в .ll.
#if defined(TB_LL) && !defined(TB_BARE)
#define TB_X(r, n, p, a) r tb_x_##n p { return tb_##n a; }
#define TB_XV(n, p, a) void tb_x_##n p { tb_##n a; }
TB_XV(ps, (const char *s), (s))
TB_X(const char *, itos, (long long v), (v))
TB_X(const char *, dtos, (double v), (v))
TB_X(const char *, btos, (int v), (v))
TB_X(const char *, digits, (long long v, char *d, long long *n), (v, d, n))
TB_X(void *, mem_np, (long long n), (n))
TB_X(long long, tlen, (const char *s), (s))
TB_X(const char *, char_at, (const char *s, long long i, long long n, int line), (s, i, n, line))
TB_X(const char *, cat, (const char *a, const char *b), (a, b))
TB_XV(app, (const char **p, long long *rl, long long *rc, const char *b), (p, rl, rc, b))
TB_XV(app_i, (const char **p, long long *rl, long long *rc, long long v), (p, rl, rc, v))
TB_XV(app_n, (const char **p, long long *rl, long long *rc, const char *b, long long n), (p, rl, rc, b, n))
TB_XV(temp_begin_p, (tb_mark *m), (m))
TB_XV(temp_end_p, (tb_mark *m), (m))
TB_X(int, fa_begin, (tb_mark *m), (m))
TB_XV(fa_end, (int fa, tb_mark *m), (fa, m))
TB_X(void *, grow, (tb_list *l), (l))
TB_X(void *, pop, (tb_list *l, int line), (l, line))
TB_XV(lclear, (tb_list *l, int line), (l, line))
TB_X(tb_list *, list_new_np, (long long n), (n))
TB_X(tb_list *, list_new, (long long n), (n))
TB_X(tb_list *, list_from_np, (long long n, long long s, const void *p), (n, s, p))
TB_X(tb_list *, list_from, (long long n, long long s, const void *p), (n, s, p))
TB_X(tb_dict *, dict_new, (int t, long long v), (t, v))
TB_X(tb_dict *, dict_from, (int t, long long v, long long n, const void *k, const void *vs), (t, v, n, k, vs))
TB_X(void *, dget, (tb_dict *d, const void *k, int line), (d, k, line))
TB_X(void *, dput, (tb_dict *d, const void *k, const void *def), (d, k, def))
TB_X(int, dhas, (tb_dict *d, const void *k), (d, k))
TB_XV(dremove, (tb_dict *d, const void *k), (d, k))
TB_XV(dclear, (tb_dict *d, int line), (d, line))
TB_X(long long, dlen, (tb_dict *d), (d))
TB_X(tb_list *, dkeys_copy, (tb_dict *d), (d))
TB_X(tb_list *, dvals_copy, (tb_dict *d), (d))
TB_XV(die, (const char *m), (m))
TB_X(const char *, chr, (long long c), (c))
TB_X(tb_list *, bits_of_bytes, (tb_list *b), (b))
TB_XV(app_take, (const char **p, long long *rl, long long *rc, const char *b, long long brl, long long brc), (p, rl, rc, b, brl, brc))
TB_X(unsigned long long, dpre1, (tb_dict *d, const void *k), (d, k))
TB_XV(dpre2, (tb_dict *d, unsigned long long i), (d, i))
TB_XV(lpre, (tb_list *l, long long i), (l, i))
TB_X(unsigned long, lock_take, (int *l), (l))
TB_XV(lock_give, (int *l, unsigned long f), (l, f))
TB_X(tb_chan *, chan_new, (long long size, int text), (size, text))
TB_XV(chan_send, (tb_chan *ch, const void *v, int line), (ch, v, line))
TB_X(int, chan_take, (tb_chan *ch, void *out), (ch, out))
TB_XV(chan_close, (tb_chan *ch), (ch))
TB_XV(par_run, (void *(*w)(void *), void *j, long long t), (w, j, t))
TB_X(long long, par_step, (long long t), (t))
void tb_x_par_lock(void) { pthread_mutex_lock(&tb_par_lock); }
void tb_x_par_unlock(void) { pthread_mutex_unlock(&tb_par_lock); }
TB_X(_Bool, bget, (tb_list *l, long long i, int line), (l, i, line))
TB_XV(bset, (tb_list *l, long long i, _Bool v, int line), (l, i, v, line))
TB_XV(badd, (tb_list *l, _Bool v), (l, v))
TB_X(_Bool, bpop, (tb_list *l, int line), (l, line))
TB_X(tb_list *, bits_new, (void), ())
TB_X(tb_list *, bits_from, (long long n, const unsigned char *s), (n, s))
const char **tb_x_errp(void) { return &tb_err; }
void tb_ll_crash(int line, int what) { tb_crash(line, what == 0 ? "деление на ноль" : what == 1 ? "остаток от деления на ноль" : what == 3 ? "номер за пределами списка" : what == 5 ? "объект пустой: его ещё не создали" : what == 6 ? "канал закрыт и пуст: receive() нечего взять" : what == 7 ? "размер куска в by должен быть больше 0" : "obj пустой: его ещё не создали"); }
void tb_ll_noreturn(int line, const char *name) { char m[256]; snprintf(m, sizeof m, "функция %s дошла до конца без return", name); tb_crash(line, m); }
// программа целиком в .ll: память obj, показ чисел, начало main, фоновые задачи
TB_X(void *, obj_mem, (long long n), (n))
TB_X(void *, obj_mem_np, (long long n), (n))
TB_X(void *, cold_mem, (long long n), (n))
TB_X(void *, cold_mem_np, (long long n), (n))
TB_X(void *, gc_alloc, (long long n), (n))
TB_X(void *, mem, (long long n), (n))
TB_X(void *, resize, (void *p, long long a, long long b), (p, a, b))
TB_XV(gc_root, (void *p, long long n), (p, n))
TB_XV(call_main, (void (*f)(void)), (f))
TB_XV(task_start, (tb_task *t, void *(*run)(void *), void *arg), (t, run, arg))
TB_X(void *, wait, (tb_task *t), (t))
void tb_x_show_i(long long x) { printf("%lld", x); }
void tb_x_show_d(double x) { printf("%g", x); }
void tb_x_start(int argc, char **argv, void *top) {
    tb_guard_init();
    tb_gc_init(top);
    tb_gc_root((void *)&tb_args, sizeof tb_args);
    tb_gc_root((void *)&tb_err, sizeof tb_err);
    tb_args = tb_list_new(sizeof(const char *));
    for (int i = 1; i < argc; i++) *(const char **)tb_grow(tb_args) = argv[i];
}
// встроенные функции языка: у каждой в конце номер строки (для сообщений об ошибке)
#define TB_B(r, n, p, a) r tb_xb_##n p { return a; }
#define TB_BV(n, p, a) void tb_xb_##n p { a; }
TB_B(long long, round, (double x, int line), tb_round(x))
TB_B(const char *, char, (long long x, int line), tb_chr(x))
TB_B(const char *, cut, (const char *s, long long a, long long b, int line), tb_cut(s, a, b, line))
TB_B(long long, code, (const char *s, int line), tb_code(s, line))
TB_B(long long, to_int, (const char *s, int line), tb_to_int(s))
TB_B(double, to_dec, (const char *s, int line), tb_to_dec(s))
TB_B(const char *, read_file, (const char *s, int line), tb_read_file(s))
TB_BV(write_file, (const char *p, const char *s, int line), tb_write_file(p, s))
TB_B(tb_list *, split, (const char *s, const char *sep, int line), tb_split(s, sep, line))
TB_B(const char *, join, (tb_list *l, const char *sep, int line), tb_join(l, sep))
TB_B(long long, find, (const char *s, const char *p, int line), tb_find(s, p))
TB_B(const char *, replace, (const char *s, const char *a, const char *b, int line), tb_replace(s, a, b, line))
TB_B(const char *, upper, (const char *s, int line), tb_upper(s))
TB_B(const char *, lower, (const char *s, int line), tb_lower(s))
TB_B(const char *, trim, (const char *s, int line), tb_trim(s))
TB_B(_Bool, starts_with, (const char *s, const char *p, int line), tb_starts(s, p))
TB_B(_Bool, ends_with, (const char *s, const char *p, int line), tb_ends(s, p))
TB_B(long long, run, (const char *s, int line), tb_run(s))
TB_BV(exit, (long long x, int line), tb_exit(x))
TB_B(_Bool, file_exists, (const char *s, int line), tb_file_exists(s))
TB_B(tb_list *, list_dir, (const char *s, int line), tb_list_dir(s))
TB_B(_Bool, is_folder, (const char *s, int line), tb_is_folder(s))
TB_BV(make_folder, (const char *s, int line), tb_make_folder(s))
TB_BV(move, (const char *a, const char *b, int line), tb_move(a, b))
TB_BV(remove, (const char *s, int line), tb_remove(s))
TB_B(const char *, env, (const char *s, int line), tb_env(s))
TB_B(long long, file_size, (const char *s, int line), tb_file_size(s))
TB_B(long long, rgb, (long long r, long long g, long long b, int line), tb_rgb(r, g, b))
TB_B(long long, random, (long long a, long long b, int line), tb_random(a, b))
TB_B(long long, bit_and, (long long a, long long b, int line), tb_band(a, b))
TB_B(long long, bit_or, (long long a, long long b, int line), tb_bor(a, b))
TB_B(long long, bit_xor, (long long a, long long b, int line), tb_bxor(a, b))
TB_B(long long, bit_not, (long long a, int line), tb_bnot(a))
TB_B(long long, shift_left, (long long a, long long b, int line), tb_shl(a, b))
TB_B(long long, shift_right, (long long a, long long b, int line), tb_shr(a, b))
TB_BV(wait_ms, (long long n, int line), tb_wait_ms(n))
TB_B(const char *, http_get, (const char *s, int line), tb_http_get(s))
TB_B(const char *, json_get, (const char *s, const char *p, int line), tb_json_get(s, p))
TB_B(const char *, json_text, (const char *s, int line), tb_json_text(s))
TB_B(const char *, date_now, (int line), tb_date_now())
TB_B(long long, unix_time, (int line), tb_unix_time())
TB_B(long long, time_ms, (int line), tb_time_ms())
TB_B(const char *, input, (int line), tb_input())
TB_B(tb_list *, args, (int line), tb_args)
TB_BV(sort_int, (tb_list *l, int line), tb_sort_list(l, tb_cmp_int))
TB_BV(sort_dec, (tb_list *l, int line), tb_sort_list(l, tb_cmp_dec))
TB_BV(sort_text, (tb_list *l, int line), tb_sort_list(l, tb_cmp_text))
TB_B(double, sqrt, (double x, int line), tb_sqrt(x, line))
TB_B(long long, read8, (long long a, int line), tb_rd8(a))
TB_BV(write8, (long long a, long long v, int line), tb_wr8(a, v))
TB_B(long long, read16, (long long a, int line), tb_rd16(a))
TB_BV(write16, (long long a, long long v, int line), tb_wr16(a, v))
TB_B(long long, read32, (long long a, int line), tb_rd32(a))
TB_BV(write32, (long long a, long long v, int line), tb_wr32(a, v))
TB_B(long long, read64, (long long a, int line), tb_rd64(a))
TB_BV(write64, (long long a, long long v, int line), tb_wr64(a, v))
TB_B(long long, address_of, (tb_list *l, int line), tb_addr_of(l))
TB_B(long long, abs_i, (long long x, int line), tb_absi(x))
TB_B(double, abs_d, (double x, int line), fabs(x))
TB_B(long long, min_i, (long long a, long long b, int line), tb_mini(a, b))
TB_B(long long, max_i, (long long a, long long b, int line), tb_maxi(a, b))
TB_B(double, min_d, (double a, double b, int line), tb_mind(a, b))
TB_B(double, max_d, (double a, double b, int line), tb_maxd(a, b))
#ifdef TB_GFX
TB_BV(window_open, (long long w, long long h, const char *t, int line), tb_window_open(w, h, t))
TB_B(_Bool, window_alive, (int line), tb_window_alive())
TB_BV(clear_window, (long long c, int line), tb_clear_window(c))
TB_BV(draw_pixel, (long long x, long long y, long long c, int line), tb_draw_pixel(x, y, c))
TB_BV(draw_rect, (long long x, long long y, long long w, long long h, long long c, int line), tb_draw_rect(x, y, w, h, c))
TB_BV(draw_circle, (long long x, long long y, long long r, long long c, int line), tb_draw_circle(x, y, r, c))
TB_BV(draw_line, (long long x0, long long y0, long long x1, long long y1, long long c, int line), tb_draw_line(x0, y0, x1, y1, c))
TB_BV(draw_text, (long long x, long long y, const char *s, long long n, long long c, int line), tb_draw_text(x, y, s, n, c))
TB_B(_Bool, key_down, (const char *s, int line), tb_key_down(s))
TB_B(long long, mouse_x, (int line), tb_mouse_x())
TB_B(long long, mouse_y, (int line), tb_mouse_y())
TB_B(_Bool, mouse_down, (int line), tb_mouse_down())
#endif
#endif
