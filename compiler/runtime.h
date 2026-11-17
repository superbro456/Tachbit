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

static inline void tb_outb(unsigned short port, unsigned char v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline unsigned char tb_inb(unsigned short port) { unsigned char v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void tb_outw(unsigned short port, unsigned short v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline unsigned short tb_inw(unsigned short port) { unsigned short v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void tb_outl(unsigned short port, unsigned int v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline unsigned int tb_inl(unsigned short port) { unsigned int v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
// выключить прерывания на время короткого дела и вернуть, как было
static inline uint32_t tb_irq_save(void) { uint32_t f; __asm__ volatile("pushfl\npopl %0\ncli" : "=r"(f) : : "memory"); return f; }
static inline void tb_irq_restore(uint32_t f) { __asm__ volatile("pushl %0\npopfl" : : "r"(f) : "memory", "cc"); }

// экран 80×25: байт буквы и байт цвета; русские буквы рисуем своим шрифтом (см. tb_vga_font)
static volatile unsigned short *const tb_vga = (volatile unsigned short *)0xB8000;
static int tb_vrow = 0, tb_vcol = 0;
static unsigned char tb_vcolor = 0x0A;   // светло-зелёный на чёрном
static void tb_vga_put(unsigned char ch) {
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
static const unsigned char tb_cyr_font[66][16] = {
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
static void tb_vga_font(void) {
    tb_outw(0x3C4, 0x0402); tb_outw(0x3C4, 0x0704);   // пишем в слой шрифта
    tb_outw(0x3CE, 0x0204); tb_outw(0x3CE, 0x0005); tb_outw(0x3CE, 0x0406);
    volatile unsigned char *font = (volatile unsigned char *)0xA0000;
    for (int i = 0; i < 66; i++)
        for (int y = 0; y < 16; y++) font[(128 + i) * 32 + y] = tb_cyr_font[i][y];
    tb_outw(0x3C4, 0x0302); tb_outw(0x3C4, 0x0304);   // обратно в текстовый режим
    tb_outw(0x3CE, 0x0004); tb_outw(0x3CE, 0x1005); tb_outw(0x3CE, 0x0E06);
}
static unsigned char tb_utf_lead = 0;
static void tb_putc(char c) {
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
static int fputs(const char *s, FILE *f) {
    uint32_t fl = tb_irq_save();
    unsigned char was = tb_vcolor;
    if (f == stderr) tb_vcolor = 0x0C;   // ошибки — красным
    while (*s) tb_putc(*s++);
    tb_vcolor = was;
    tb_irq_restore(fl);
    return 0;
}
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
static unsigned char tb_heap[TB_HEAP] __attribute__((aligned(16), section(".tbheap")));
static size_t tb_heap_top = 0;
static void *tb_bins[32];
static int tb_cls(size_t n) { int k = 4; while (k < 31 && ((size_t)1 << k) < n) k++; return k; }
static void *tb_malloc0(size_t n) {
    int k = tb_cls(n);
    if (tb_bins[k]) { void *p = tb_bins[k]; tb_bins[k] = *(void **)p; return p; }
    size_t need = ((size_t)1 << k) + 16;
    if (tb_heap_top + need > TB_HEAP) return 0;
    unsigned char *h = tb_heap + tb_heap_top;
    *(size_t *)h = (size_t)k;
    tb_heap_top += need;
    return h + 16;
}
static void tb_free0(void *p) {
    if (p == 0) return;
    unsigned char *h = (unsigned char *)p - 16;
    int k = (int)*(size_t *)h;
    if (h + ((size_t)1 << k) + 16 == tb_heap + tb_heap_top) { tb_heap_top = (size_t)(h - tb_heap); return; }
    *(void **)p = tb_bins[k];
    tb_bins[k] = p;
}
// задачи и прерывания тоже берут память: на время выдачи прерывания выключены
static void *malloc(size_t n) { uint32_t f = tb_irq_save(); void *p = tb_malloc0(n); tb_irq_restore(f); return p; }
static void free(void *p) { uint32_t f = tb_irq_save(); tb_free0(p); tb_irq_restore(f); }
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
static void *tb_realloc0(void *p, size_t n) {
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
static void *realloc(void *p, size_t n) { uint32_t f = tb_irq_save(); void *q = tb_realloc0(p, n); tb_irq_restore(f); return q; }
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
// ---------- ядро: прерывания, таймер, клавиатура, задачи, страницы памяти ----------
// on all cores здесь выполняется на одном ядре по очереди (как и было),
// а background — настоящая задача: таймер переключает задачи каждые 10 мс.
static long sysconf(int n) { (void)n; return 1; }
static int pthread_create(pthread_t *t, void *a, void *(*run)(void *), void *arg) { (void)t; (void)a; run(arg); return 0; }
static int pthread_join(pthread_t t, void **r) { (void)t; (void)r; return 0; }

// адреса из файла сборки (link.ld): где код, где кончается нижняя память, где куча
extern char tb_code_start[], tb_code_end[], tb_tls_start[], tb_tls_end[], tb_heap_area[], tb_kernel_end[];
static unsigned char tb_stack[1 << 20] __attribute__((aligned(4096), used));
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
static uint64_t tb_gdt[5] = {0, 0x00CF9A000000FFFFULL, 0x00CF92000000FFFFULL, 0, 0};
// двойная ошибка (например, стек упёрся в сторожа и обработчику некуда класть) идёт
// через отдельную «задачу процессора» со своим стеком — так ядро успевает сказать, что случилось
static uint32_t tb_tss_main[26], tb_tss_df[26];
static unsigned char tb_df_stack[8192] __attribute__((aligned(16)));
static uint64_t tb_tss_desc(uint32_t base) {
    return 103ull | ((uint64_t)(base & 0xFFFFFFu) << 16) | (0x89ull << 40) | ((uint64_t)(base >> 24) << 56);
}
static uint64_t tb_idt[256];
static struct __attribute__((packed)) { uint16_t lim; uint32_t base; } tb_gdtr, tb_idtr;

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
static tb_kslot tb_slots[TB_NTASK];          // 0 — main
static unsigned char tb_tstacks[TB_NTASK][TB_TSTACK] __attribute__((aligned(4096)));
static int tb_cur = 0, tb_nlive = 1, tb_slice = 10, tb_in_irq = 0;
static unsigned char tb_tls_zero[TB_TLS_MAX], tb_fpu_init[108];
static unsigned char tb_irq_tls[TB_TLS_MAX];
static volatile uint64_t tb_ms = 0;
static void (*tb_irq_fn[16])(void);
static uint16_t tb_pic_mask = 0xFFFF;
static uint32_t tb_tls_size(void) { return (uint32_t)(tb_tls_end - tb_tls_start); }

static void tb_pic_apply(void) { tb_outb(0x21, (unsigned char)tb_pic_mask); tb_outb(0xA1, (unsigned char)(tb_pic_mask >> 8)); }
static void tb_irq_set(int v, void (*fn)(void)) {
    tb_irq_fn[v - 32] = fn;
    tb_pic_mask &= (uint16_t)~(1u << (v - 32));
    if (v >= 40) tb_pic_mask &= (uint16_t)~(1u << 2);
    tb_pic_apply();
}
static void tb_irq_on(void) { __asm__ volatile("sti" ::: "memory"); }
static void tb_irq_off(void) { __asm__ volatile("cli" ::: "memory"); }
static void tb_yield(void) { if (!tb_in_irq) __asm__ volatile("int $48" ::: "memory"); }

// переключение задач: сохранить «переменные потока» и дроби текущей, взять следующей
static uint32_t tb_schedule(uint32_t esp) {
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
static void tb_task_entry(void) {
    tb_kslot *s = &tb_slots[tb_cur];
    s->run(s->arg);
    __asm__ volatile("cli");
    s->t->done = 1;
    s->done = 1;
    for (;;) __asm__ volatile("int $48");
}
static void tb_task_start(tb_task *t, void *(*run)(void *), void *arg) {
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
static void *tb_wait(tb_task *t) {
    if (tb_in_irq && !t->done) tb_panic("wait внутри прерывания нельзя", 0, 0);
    while (!t->done) tb_yield();
    return t->res;
}

// --- время ---
static long long tb_uptime_ms(void) { uint32_t f = tb_irq_save(); uint64_t v = tb_ms; tb_irq_restore(f); return (long long)v; }
static void tb_wait_ms(long long n) {
    if (tb_in_irq) tb_panic("wait_ms внутри прерывания нельзя: прерывание должно быть коротким", 0, 0);
    long long end = tb_uptime_ms() + n;
    while (tb_uptime_ms() < end) {
        if (tb_nlive > 1) tb_yield();
        else __asm__ volatile("sti; hlt" ::: "memory");
    }
}

// --- клавиатура: буквы копятся в очереди, read_key() берёт по одной ---
static const char tb_kmap[2][58] = {
    {0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
     'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
     'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
     'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '},
    {0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
     'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
     'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
     'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '}};
static char tb_keys[256];
static volatile unsigned char tb_khead = 0, tb_ktail = 0;
static int tb_kshift = 0;
static char tb_ktext[256][2];
static void tb_key_irq(void) {
    unsigned char sc = tb_inb(0x60);
    if (sc == 0x2A || sc == 0x36) { tb_kshift = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { tb_kshift = 0; return; }
    if (sc >= 58) return;
    char ch = tb_kmap[tb_kshift][sc];
    if (ch == 0 || (unsigned char)(tb_khead + 1) == tb_ktail) return;
    tb_keys[tb_khead] = ch;
    tb_khead = (unsigned char)(tb_khead + 1);
}
static const char *tb_read_key(void) {
    uint32_t f = tb_irq_save();
    const char *r = "";
    if (tb_ktail != tb_khead) { r = tb_ktext[(unsigned char)tb_keys[tb_ktail]]; tb_ktail = (unsigned char)(tb_ktail + 1); }
    tb_irq_restore(f);
    return r;
}

// --- страницы памяти: адрес 0 и код защищены, у стеков — пустая страница-сторож ---
#define TB_LOW_TABLES 8
static uint32_t tb_pd[1024] __attribute__((aligned(4096)));
static uint32_t tb_pt[TB_LOW_TABLES][1024] __attribute__((aligned(4096)));
static uint32_t tb_ram_end = 0, tb_pg_next = 0, tb_pg_free = 0;
static void tb_unmap(uint32_t a) { tb_pt[a >> 22][(a >> 12) & 1023] = 0; }
static int tb_is_guard(uint32_t a) {
    if (a >= (uint32_t)tb_stack && a < (uint32_t)tb_stack + 4096) return 1;
    for (int i = 1; i < TB_NTASK; i++)
        if (a >= (uint32_t)tb_tstacks[i] && a < (uint32_t)tb_tstacks[i] + 4096) return 1;
    return 0;
}
static void tb_paging_init(void) {
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
static long long tb_page_new(void) {
    uint32_t f = tb_irq_save(), p = 0;
    if (tb_pg_free) { p = tb_pg_free; tb_pg_free = *(uint32_t *)p; }
    else if (tb_pg_next + 4096 <= tb_ram_end) { p = tb_pg_next; tb_pg_next += 4096; }
    tb_irq_restore(f);
    if (p == 0) tb_panic("свободные страницы памяти кончились", tb_ram_end, 0);
    memset((void *)p, 0, 4096);
    return p;
}
static void tb_page_free(long long a) {
    uint32_t p = (uint32_t)a;
    if ((p & 4095) || p < (uint32_t)tb_kernel_end || p >= tb_pg_next) tb_panic("page_free: это не страница из page_new", p, 0);
    uint32_t f = tb_irq_save();
    *(uint32_t *)p = tb_pg_free;
    tb_pg_free = p;
    tb_irq_restore(f);
}

// --- ошибки процессора ---
static const char *tb_fault_name[32] = {"деление на ноль", "отладка", "NMI", "точка останова", "переполнение",
    "выход за границы", "неизвестная команда", "нет FPU", "двойная ошибка", "", "плохой TSS", "нет сегмента",
    "ошибка стека", "нарушение защиты", "ошибка страницы", "", "ошибка дробей", "выравнивание", "проверка железа", "ошибка SIMD"};
static void tb_fault(uint32_t v, uint32_t *f) {
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
static const unsigned int tb_multiboot[3] = {0x1BADB002u, 2u, (unsigned int)-(0x1BADB002 + 2)};
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
#ifndef TB_BARE
// ---------- падение по памяти: понятное сообщение вместо «Segmentation fault» ----------
// Чаще всего это стек: функция зовёт себя слишком глубоко. Обработчик работает
// на своём маленьком стеке (sigaltstack), потому что основной уже кончился.
#include <signal.h>
#include <sys/resource.h>
// у каждого потока своё: где начинается его стек и сколько в нём места
static _Thread_local char *tb_main_sp;
static _Thread_local long long tb_stack_lim;
#define TB_THREAD_STACK (8LL << 20)
static void tb_on_segv(int sig, siginfo_t *si, void *u) {
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
// свой маленький стек для сообщения: у главного потока — постоянный, у остальных — из malloc
static void tb_guard_thread(char *here, long long lim) {
    stack_t ss;
    ss.ss_sp = malloc(65536); ss.ss_size = 65536; ss.ss_flags = 0;
    if (ss.ss_sp) sigaltstack(&ss, 0);
    tb_main_sp = here;
    tb_stack_lim = lim;
}
// поток background или ядра: сначала ставим защиту стека, потом сама работа
typedef struct { void *(*run)(void *); void *arg; } tb_tramp;
static void *tb_tramp_run(void *p) {
    tb_tramp t = *(tb_tramp *)p;
    free(p);
    char here;
    tb_guard_thread(&here, TB_THREAD_STACK);
    void *r = t.run(t.arg);
    stack_t ss, old;
    ss.ss_sp = 0; ss.ss_size = 0; ss.ss_flags = SS_DISABLE;
    if (sigaltstack(&ss, &old) == 0 && old.ss_sp) free(old.ss_sp);
    return r;
}
// поток с 8 МБ стека (на Mac по умолчанию всего 512 КБ) и с понятным сообщением при переполнении
static int tb_thread_create(pthread_t *th, void *(*run)(void *), void *arg) {
    tb_tramp *t = malloc(sizeof(tb_tramp));
    if (!t) return -1;
    t->run = run; t->arg = arg;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, (size_t)TB_THREAD_STACK);
    int r = pthread_create(th, &at, tb_tramp_run, t);
    pthread_attr_destroy(&at);
    if (r != 0) free(t);
    return r;
}
static void tb_guard_init(void) {
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
// ---------- папки (в ядре без ОС их нет) ----------
#ifndef TB_BARE
static int tb_name_cmp(const void *a, const void *b) {
    const char *x = *(const char * const *)a, *y = *(const char * const *)b;
    size_t lx = strlen(x), ly = strlen(y);
    int dx = lx > 0 && x[lx - 1] == '/', dy = ly > 0 && y[ly - 1] == '/';
    if (dx != dy) return dy - dx;          // папки первыми
    return strcasecmp(x, y);
}
static int tb_is_folder(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static tb_list *tb_list_dir(const char *path) {
    tb_list *l = tb_list_new(sizeof(const char *));
    DIR *d = opendir(path);
    if (d == 0) { tb_err = tb_cat("не могу открыть папку: ", path); return l; }
    struct dirent *e;
    size_t lp = strlen(path);
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        size_t n = strlen(e->d_name);
        char *full = tb_mem(lp + n + 2);
        memcpy(full, path, lp); full[lp] = '/'; memcpy(full + lp + 1, e->d_name, n + 1);
        int dir = tb_is_folder(full);
        char *name = tb_mem(n + 2);
        memcpy(name, e->d_name, n);
        if (dir) name[n++] = '/';
        name[n] = 0;
        *(const char **)tb_grow(l) = name;
    }
    closedir(d);
    if (l->len > 1) qsort(l->data, l->len, sizeof(const char *), tb_name_cmp);
    return l;
}
static void tb_make_folder(const char *path) {
    if (mkdir(path, 0755) != 0) tb_err = tb_cat(errno == EEXIST ? "уже есть: " : "не могу создать папку: ", path);
}
static void tb_move(const char *from, const char *to) {
    if (rename(from, to) != 0) tb_err = tb_cat(errno == EXDEV ? "нельзя переместить на другой диск: " : "не могу переместить: ", from);
}
static void tb_remove(const char *path) {
    int r = tb_is_folder(path) ? rmdir(path) : unlink(path);
    if (r != 0) tb_err = tb_cat(errno == ENOTEMPTY || errno == EEXIST ? "папка не пустая: " : "не могу удалить: ", path);
}
static const char *tb_env(const char *name) {
    const char *v = getenv(name);
    return v ? v : "";
}
static long long tb_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long)st.st_size;
}
// ---------- время и случайные числа ----------
static long long tb_time_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void tb_wait_ms(long long n) { fflush(stdout); if (n > 0) usleep((useconds_t)(n * 1000)); }
static unsigned long long tb_rand_state = 0;
// цвет из красного, зелёного и синего (0..255) — одно число 0xRRGGBB
static long long tb_rgb(long long r, long long g, long long b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (r << 16) | (g << 8) | b;
}
// случайное целое от a до b (оба входят)
static long long tb_random(long long a, long long b) {
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
// ---------- obj: память пачкой ----------
// obj никогда не отдают по одному (нет free), поэтому их не надо брать через malloc
// поштучно: берём кусок на тысячи объектов и раздаём подряд. Быстрее, и соседние
// объекты лежат рядом в памяти. Внутри temp и шага цикла — как раньше, из временной памяти.
static void *tb_obj_mem(long long n) {
    if (tb_temp_depth > 0) return tb_mem(n);
    static _Thread_local char *p = 0;
    static _Thread_local long long left = 0;
    n = (n + 7) & ~7LL;
    if (left < n) {
        long long size = n > (1LL << 20) ? n : (1LL << 20);
        p = malloc(size);
        if (!p) tb_crash(0, "не хватило памяти");
        left = size;
    }
    void *r = p;
    p += n;
    left -= n;
    return r;
}
// ---------- background: работа в своём потоке ----------
#ifndef TB_BARE
typedef struct { pthread_t th; int done; void *res; } tb_task;
static void tb_task_start(tb_task *t, void *(*run)(void *), void *arg) {
    t->done = 0;
    if (tb_thread_create(&t->th, run, arg) != 0) tb_crash(0, "не получилось запустить background");
}
// a.wait: дождаться потока (один раз) и отдать адрес ответа
static void *tb_wait(tb_task *t) {
    if (!t->done) { pthread_join(t->th, 0); t->done = 1; }
    return t->res;
}
#endif
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
#ifdef TB_BARE
        if (pthread_create(&t[i], 0, work, job) != 0) tb_crash(0, "не получилось запустить ядро");
#else
        if (tb_thread_create(&t[i], work, job) != 0) tb_crash(0, "не получилось запустить ядро");
#endif
    }
    work(job);
    for (int i = 1; i < n; i++) pthread_join(t[i], 0);
}
static long long tb_par_step(long long total) {
    long long s = total / ((long long)tb_cores() * 16);
    return s < 1 ? 1 : s;
}

// ---------- биты, адреса, порты, замки (danger и lock) ----------
static inline long long tb_band(long long a, long long b) { return a & b; }
static inline long long tb_bor(long long a, long long b) { return a | b; }
static inline long long tb_bxor(long long a, long long b) { return a ^ b; }
static inline long long tb_bnot(long long a) { return ~a; }
static inline long long tb_shl(long long a, long long n) { return (long long)((unsigned long long)a << (n & 63)); }
static inline long long tb_shr(long long a, long long n) { return (long long)((unsigned long long)a >> (n & 63)); }
// чтение и запись по адресу: volatile — компилятор C не выкинет и не переставит их
static inline long long tb_rd8(long long a) { return *(volatile uint8_t *)(uintptr_t)a; }
static inline long long tb_rd16(long long a) { return *(volatile uint16_t *)(uintptr_t)a; }
static inline long long tb_rd32(long long a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline long long tb_rd64(long long a) { return (long long)*(volatile uint64_t *)(uintptr_t)a; }
static inline void tb_wr8(long long a, long long v) { *(volatile uint8_t *)(uintptr_t)a = (uint8_t)v; }
static inline void tb_wr16(long long a, long long v) { *(volatile uint16_t *)(uintptr_t)a = (uint16_t)v; }
static inline void tb_wr32(long long a, long long v) { *(volatile uint32_t *)(uintptr_t)a = (uint32_t)v; }
static inline void tb_wr64(long long a, long long v) { *(volatile uint64_t *)(uintptr_t)a = (uint64_t)v; }
static inline long long tb_addr_of(tb_list *l) { return (long long)(uintptr_t)l->data; }
#ifdef TB_BARE
static inline long long tb_pin8(long long p) { return tb_inb((unsigned short)p); }
static inline long long tb_pin16(long long p) { return tb_inw((unsigned short)p); }
static inline long long tb_pin32(long long p) { return tb_inl((unsigned short)p); }
static inline void tb_pout8(long long p, long long v) { tb_outb((unsigned short)p, (unsigned char)v); }
static inline void tb_pout16(long long p, long long v) { tb_outw((unsigned short)p, (unsigned short)v); }
static inline void tb_pout32(long long p, long long v) { tb_outl((unsigned short)p, (unsigned int)v); }
// замок в ядре: одна команда xchg + прерывания выключены, пока замок у нас
// (так прерывание не застрянет на замке, который держит прерванная задача)
static inline unsigned long tb_lock_take(int *l) {
    uint32_t f = tb_irq_save();
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) { tb_irq_restore(f); tb_yield(); f = tb_irq_save(); }
    return f;
}
static inline void tb_lock_give(int *l, unsigned long f) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); tb_irq_restore((uint32_t)f); }
#else
// замок в программе: атомарный обмен; занято — сначала коротко ждём, потом уступаем ядро
static inline unsigned long tb_lock_take(int *l) {
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
static inline void tb_lock_give(int *l, unsigned long f) { (void)f; __atomic_store_n(l, 0, __ATOMIC_RELEASE); }
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
static tb_chan *tb_chan_new(long long size, int text) {
    tb_chan *ch = malloc(sizeof(tb_chan));
    if (!ch) tb_crash(0, "не хватило памяти");
    memset(ch, 0, sizeof(tb_chan));
#ifndef TB_BARE
    pthread_mutex_init(&ch->m, 0); pthread_cond_init(&ch->cv, 0);
#endif
    ch->size = size; ch->text = text; ch->cap = 16;
    ch->buf = malloc(ch->cap * size);
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
static void tb_chan_send(tb_chan *ch, const void *v, int line) {
    char tmp[16];
    const void *src = v;
    if (ch->text) {
        const char *t = *(const char * const *)v;
        size_t n = strlen(t) + 1;
        char *copy = malloc(n);
        if (!copy) tb_crash(line, "не хватило памяти");
        memcpy(copy, t, n);
        memcpy(tmp, &copy, sizeof copy);
        src = tmp;
    }
    TB_CH_LOCK(ch);
    if (ch->closed) { TB_CH_UNLOCK(ch); tb_crash(line, "канал уже закрыт: в него больше не пишут"); }
    if (ch->len == ch->cap) {
        char *nb = malloc(ch->cap * 2 * ch->size);
        if (!nb) { TB_CH_UNLOCK(ch); tb_crash(line, "не хватило памяти"); }
        for (long long i = 0; i < ch->len; i++)
            memcpy(nb + i * ch->size, ch->buf + ((ch->head + i) % ch->cap) * ch->size, ch->size);
        free(ch->buf); ch->buf = nb; ch->head = 0; ch->cap *= 2;
    }
    memcpy(ch->buf + ((ch->head + ch->len) % ch->cap) * ch->size, src, ch->size);
    ch->len++;
#ifndef TB_BARE
    pthread_cond_signal(&ch->cv);
#endif
    TB_CH_UNLOCK(ch);
}
// взять следующее значение; 0 — канал закрыт и пуст
static int tb_chan_take(tb_chan *ch, void *out) {
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
static void tb_chan_close(tb_chan *ch) {
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
static const unsigned char tb_font[161][16] = {
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
static uint32_t *tb_px = 0;
static int tb_gw = 0, tb_gh = 0, tb_gclosed = 0, tb_mx = 0, tb_my = 0, tb_mdown = 0;
static unsigned char tb_keys[256];
static long long tb_last_frame = 0;
// клавиши: буквы и цифры — свой код ASCII (A..Z, 0..9), остальные:
enum { TB_K_ENTER = 10, TB_K_ESC = 27, TB_K_SPACE = 32, TB_K_BACK = 8, TB_K_TAB = 9,
       TB_K_UP = 17, TB_K_DOWN = 18, TB_K_RIGHT = 19, TB_K_LEFT = 20, TB_K_SHIFT = 21 };
#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#include <CoreGraphics/CoreGraphics.h>
#define TB_MSG(R, o, s) ((R(*)(id, SEL))objc_msgSend)((id)(o), sel_getUid(s))
#define TB_MSG1(R, o, s, A, a) ((R(*)(id, SEL, A))objc_msgSend)((id)(o), sel_getUid(s), a)
#define TB_CLS(n) ((id)objc_getClass(n))
static id tb_nsapp = 0, tb_wnd = 0;
static const unsigned char tb_mac_keys[128] = {
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
static CGColorSpaceRef tb_cgspace = 0;
static uint32_t *tb_pres[3];
static int tb_pres_i = 0;
static id tb_layer = 0;
static BOOL tb_nswnd_close(id v, SEL s, id w) { (void)v; (void)s; (void)w; tb_gclosed = 1; return YES; }
static int tb_gfx_open(const char *title) {
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
    TB_MSG1(void, view, "setWantsLayer:", BOOL, YES);
    tb_layer = TB_MSG(id, view, "layer");
    id nearest = TB_MSG1(id, TB_CLS("NSString"), "stringWithUTF8String:", const char *, "nearest");
    TB_MSG1(void, tb_layer, "setMagnificationFilter:", id, nearest);
    CGColorSpaceRef space = CGDisplayCopyColorSpace(CGMainDisplayID());
    if (!space) space = CGColorSpaceCreateDeviceRGB();
    tb_cgspace = space;
    for (int i = 0; i < 3; i++) {
        tb_pres[i] = malloc((size_t)tb_gw * tb_gh * 4);
        if (!tb_pres[i]) return 0;
    }
    TB_MSG1(void, tb_wnd, "setTitle:", id, TB_MSG1(id, TB_CLS("NSString"), "stringWithUTF8String:", const char *, title));
    TB_MSG1(void, tb_wnd, "setAcceptsMouseMovedEvents:", BOOL, YES);
    TB_MSG1(void, tb_wnd, "makeKeyAndOrderFront:", id, 0);
    TB_MSG(void, tb_wnd, "center");
    TB_MSG(void, tb_nsapp, "finishLaunching");
    TB_MSG1(void, tb_nsapp, "activateIgnoringOtherApps:", BOOL, YES);
    return 1;
}
static void tb_gfx_events(void) {
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
}
#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
static Display *tb_dpy = 0;
static Window tb_xw;
static GC tb_xgc;
static XImage *tb_ximg = 0;
static Atom tb_wm_del;
static int tb_gfx_open(const char *title) {
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
    return 1;
}
static int tb_x_key(KeySym s) {
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
static void tb_gfx_events(void) {
    XPutImage(tb_dpy, tb_xw, tb_xgc, tb_ximg, 0, 0, 0, 0, tb_gw, tb_gh);
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
static void tb_window_open(long long w, long long h, const char *title) {
    if (tb_px) { tb_err = "окно уже открыто"; return; }
    if (w < 1 || h < 1 || w > 8192 || h > 8192) { tb_err = "размер окна от 1 до 8192"; return; }
    tb_gw = (int)w; tb_gh = (int)h;
    tb_px = calloc((size_t)tb_gw * tb_gh, 4);
    if (!tb_gfx_open(title)) { free(tb_px); tb_px = 0; tb_err = "не могу открыть окно: нет экрана"; return; }
    tb_last_frame = tb_time_ms();
}
// показать кадр, принять клавиши и мышь, держать 60 кадров в секунду; false — окно закрыли
static int tb_window_alive(void) {
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
static inline void tb_put(long long x, long long y, long long c) {
    if (tb_px && x >= 0 && y >= 0 && x < tb_gw && y < tb_gh) tb_px[y * tb_gw + x] = (uint32_t)c;
}
static void tb_clear_window(long long c) { if (tb_px) for (long long i = 0; i < (long long)tb_gw * tb_gh; i++) tb_px[i] = (uint32_t)c; }
static void tb_draw_pixel(long long x, long long y, long long c) { tb_put(x, y, c); }
static void tb_draw_rect(long long x, long long y, long long w, long long h, long long c) {
    if (!tb_px) return;
    long long x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > tb_gw ? tb_gw : x + w, y1 = y + h > tb_gh ? tb_gh : y + h;
    for (long long j = y0; j < y1; j++) for (long long i = x0; i < x1; i++) tb_px[j * tb_gw + i] = (uint32_t)c;
}
static void tb_draw_circle(long long cx, long long cy, long long r, long long c) {
    for (long long j = -r; j <= r; j++) for (long long i = -r; i <= r; i++)
        if (i * i + j * j <= r * r + r) tb_put(cx + i, cy + j, c);
}
static void tb_draw_line(long long x0, long long y0, long long x1, long long y1, long long c) {
    long long dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    long long sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    while (1) {
        tb_put(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        long long e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
// текст: буква 8×16, size — во сколько раз крупнее
static void tb_draw_text(long long x, long long y, const char *s, long long size, long long c) {
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
        for (int row = 0; row < 16; row++) {
            unsigned char bits = tb_font[g][row];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col)) tb_draw_rect(cx + col * size, y + row * size, size, size, c);
        }
        cx += 8 * size;
    }
}
static int tb_key_down(const char *name) {
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
static long long tb_mouse_x(void) { return tb_mx; }
static long long tb_mouse_y(void) { return tb_my; }
static int tb_mouse_down(void) { return tb_mdown; }
#endif
