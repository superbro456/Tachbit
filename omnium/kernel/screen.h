// Экран и мышь Омниума (tb bare + рисование): видеокарта QEMU/Bochs, мышь PS/2 и vmmouse.
// Вставляется в compiler/runtime.h на место строки //@include (tools/embed_runtime.py).

// ---------- экран и мышь в ядре (tb bare) ----------
// Экран: видеокарта Bochs/QEMU (BGA, «-vga std»): режим w×h, 32 бита на точку, буфер кадра в памяти.
// Где буфер — говорит шина PCI (BAR0). Кадр рисуется в tb_px, на экран копируется целиком.
// Мышь PS/2 (прерывание 12), клавиатура — та же, что для read_key.
TB_API volatile uint32_t *tb_lfb = 0;
TB_API int tb_bcur = 0;          // курсор: 0 стрелка ... 7 спрятать
TB_API char *tb_bclip = 0;       // буфер обмена — просто текст в памяти ядра
TB_API unsigned tb_pci_r(int bus, int dev, int fn, int off) {
    tb_outl(0xCF8, 0x80000000u | (unsigned)bus << 16 | (unsigned)dev << 11 | (unsigned)fn << 8 | (unsigned)(off & 0xFC));
    return tb_inl(0xCFC);
}
TB_API uint64_t tb_bga_find(void) {
    for (int bus = 0; bus < 8; bus++)
        for (int dev = 0; dev < 32; dev++) {
            unsigned id = tb_pci_r(bus, dev, 0, 0);
            if (id == 0x11111234u || id == 0xBEEF80EEu) return tb_pci_r(bus, dev, 0, 0x10) & ~0xFu;
        }
    return 0;
}
TB_API void tb_bga_w(unsigned short i, unsigned short v) { tb_outw(0x1CE, i); tb_outw(0x1CF, v); }
TB_API unsigned short tb_bga_r(unsigned short i) { tb_outw(0x1CE, i); return tb_inw(0x1CF); }
// окно устройства в страницы (по 2 МБ, писать можно, без кэша чтения)
TB_API void tb_map_dev(uint64_t a, uint64_t n) {
    for (uint64_t p = a & ~0x1FFFFFull; p < a + n; p += 0x200000ull) {
        if (p >= (64ull << 30)) return;
        tb_pdpt[p >> 30] = (uint64_t)(uintptr_t)tb_pdir[p >> 30] | 3;
        tb_pdir[p >> 30][(p >> 21) & 511] = p | 0x83 | 0x8;   // 0x8: запись сквозная — кадр сразу виден
        __asm__ volatile("invlpg (%0)" : : "r"(p) : "memory");
    }
}
// --- клавиши для key_down / typed_text (набор 1, как шлёт контроллер клавиатуры) ---
TB_API int tb_kext = 0;
TB_API int tb_sc_key(unsigned char sc, int ext) {
    if (ext) {
        switch (sc) {
        case 0x48: return TB_K_UP; case 0x50: return TB_K_DOWN; case 0x4D: return TB_K_RIGHT; case 0x4B: return TB_K_LEFT;
        case 0x53: return TB_K_DEL; case 0x47: return TB_K_HOME; case 0x4F: return TB_K_END;
        case 0x49: return TB_K_PGUP; case 0x51: return TB_K_PGDN; case 0x1D: return TB_K_CTRL; case 0x38: return TB_K_ALT;
        case 0x1C: return TB_K_ENTER;
        }
        return 0;
    }
    switch (sc) {
    case 0x1C: return TB_K_ENTER; case 0x01: return TB_K_ESC; case 0x39: return TB_K_SPACE; case 0x0E: return TB_K_BACK;
    case 0x0F: return TB_K_TAB; case 0x2A: case 0x36: return TB_K_SHIFT; case 0x1D: return TB_K_CTRL; case 0x38: return TB_K_ALT;
    case 0x57: return TB_K_F1 + 10; case 0x58: return TB_K_F1 + 11;
    }
    if (sc >= 0x3B && sc <= 0x44) return TB_K_F1 + (sc - 0x3B);
    if (sc < 58) {
        int ch = (unsigned char)tb_kmap[0][sc];
        if (ch >= 'a' && ch <= 'z') return ch - 32;
        if (ch >= '0' && ch <= '9') return ch;
    }
    return 0;
}
TB_API void tb_gfx_key(unsigned char sc) {
    if (sc == 0xE0) { tb_kext = 1; return; }
    int up = sc & 0x80, ext = tb_kext;
    tb_kext = 0;
    sc &= 0x7F;
    int k = tb_sc_key(sc, ext);
    if (k) { tb_keys[k] = !up; if (!up) tb_ktap[k] = 1; }
    if (!up && !ext && sc < 58 && !tb_keys[TB_K_CTRL]) {
        char ch = tb_kmap[tb_keys[TB_K_SHIFT] ? 1 : 0][sc];
        if ((unsigned char)ch >= 32) tb_type_cp((unsigned char)ch);
    }
}
// --- мышь PS/2: пакеты по 3 байта (по 4 — с колёсиком) ---
TB_API unsigned char tb_mpk[4];
TB_API int tb_mpn = 0, tb_mpsize = 3;
TB_API void tb_ps2_wait_in(void) { for (int i = 0; i < 100000 && (tb_inb(0x64) & 2); i++) {} }
TB_API int tb_ps2_read(void) {
    for (int i = 0; i < 100000; i++) if (tb_inb(0x64) & 1) return tb_inb(0x60);
    return -1;
}
TB_API int tb_mouse_cmd(unsigned char c) {
    tb_ps2_wait_in(); tb_outb(0x64, 0xD4);
    tb_ps2_wait_in(); tb_outb(0x60, c);
    return tb_ps2_read();   // 0xFA — принято
}
// «мышь виртуальной машины» (vmmouse в QEMU и VMware): точное место курсора, а не сдвиги —
// мышь не надо «ловить» в окне QEMU. Есть — берём её, нет — обычные пакеты PS/2
TB_API int tb_vmm = 0;
TB_API void tb_vmm_call(uint32_t cmd, uint32_t arg, uint32_t r[4]) {
    uint32_t a = 0x564D5868u, b = arg, c = cmd, d = 0x5658;
    __asm__ volatile("inl %%dx, %%eax" : "+a"(a), "+b"(b), "+c"(c), "+d"(d) : : "memory");
    r[0] = a; r[1] = b; r[2] = c; r[3] = d;
}
TB_API void tb_vmm_init(void) {
    uint32_t r[4];
    tb_vmm_call(10, 0, r);                               // «кто здесь»: в ответе ebx — тот же магический номер
    if (r[1] != 0x564D5868u) return;
    tb_vmm_call(41, 0x45414552u, r);                     // включить
    tb_vmm_call(40, 0, r);
    if (r[0] == 0xFFFF0000u || (r[0] & 0xFFFF) == 0) return;
    tb_vmm_call(39, 1, r);
    if (r[0] != 0x3442554Au) return;
    tb_vmm_call(41, 0x53424152u, r);                     // точные координаты
    tb_vmm = 1;
}
TB_API void tb_vmm_read(void) {
    uint32_t r[4];
    for (;;) {
        tb_vmm_call(40, 0, r);
        if (r[0] == 0xFFFF0000u) { tb_vmm = 0; return; }
        if ((r[0] & 0xFFFF) < 4) return;
        tb_vmm_call(39, 4, r);
        tb_mx = (int)((uint64_t)r[1] * (uint64_t)tb_gw / 65536);
        tb_my = (int)((uint64_t)r[2] * (uint64_t)tb_gh / 65536);
        int down = (r[0] & 0x20) != 0;
        if (down && !tb_mdown) tb_mtap = 1;
        tb_mdown = down;
        int w = (int)(signed char)(r[3] & 0xFF);
        if (w) tb_wheel_add(-w);
    }
}
TB_API void tb_mouse_irq(void) {
    unsigned char st = tb_inb(0x64);
    if (!(st & 1)) return;
    unsigned char b = tb_inb(0x60);
    if (!(st & 0x20)) { tb_key_byte(b); return; }   // байт клавиатуры пришёл сюда
    if (tb_vmm) { tb_vmm_read(); return; }
    if (tb_mpn == 0 && !(b & 8)) return;           // первый байт пакета всегда с битом 3
    tb_mpk[tb_mpn++] = b;
    if (tb_mpn < tb_mpsize) return;
    tb_mpn = 0;
    int dx = tb_mpk[1] - ((tb_mpk[0] << 4) & 0x100), dy = tb_mpk[2] - ((tb_mpk[0] << 3) & 0x100);
    if (tb_mpk[0] & 0xC0) dx = dy = 0;            // переполнение: пакет не верен
    tb_mx += dx; tb_my -= dy;
    if (tb_mx < 0) tb_mx = 0;
    if (tb_my < 0) tb_my = 0;
    if (tb_mx >= tb_gw) tb_mx = tb_gw - 1;
    if (tb_my >= tb_gh) tb_my = tb_gh - 1;
    int down = tb_mpk[0] & 1;
    if (down && !tb_mdown) tb_mtap = 1;
    tb_mdown = down;
    if (tb_mpsize == 4) {
        int w = tb_mpk[3] & 0x0F;
        if (w & 8) w -= 16;
        if (w) tb_wheel_add(-w);
    }
}
TB_API void tb_mouse_init(void) {
    uint64_t f = tb_irq_save();
    tb_ps2_wait_in(); tb_outb(0x64, 0xA8);        // включить вход мыши
    tb_ps2_wait_in(); tb_outb(0x64, 0x20);
    int cfg = tb_ps2_read();
    if (cfg >= 0) {
        cfg = (cfg | 2) & ~0x20;                   // прерывание мыши есть, часы мыши идут
        tb_ps2_wait_in(); tb_outb(0x64, 0x60);
        tb_ps2_wait_in(); tb_outb(0x60, (unsigned char)cfg);
    }
    tb_mouse_cmd(0xF6);
    // колёсико: «волшебные» частоты 200, 100, 80, потом мышь называет себя 3
    tb_mouse_cmd(0xF3); tb_mouse_cmd(200); tb_mouse_cmd(0xF3); tb_mouse_cmd(100); tb_mouse_cmd(0xF3); tb_mouse_cmd(80);
    if (tb_mouse_cmd(0xF2) == 0xFA && tb_ps2_read() == 3) tb_mpsize = 4;
    tb_mouse_cmd(0xF4);                            // слать движения
    tb_vmm_init();
    while (tb_inb(0x64) & 1) tb_inb(0x60);
    tb_irq_restore(f);
    tb_irq_set(44, tb_mouse_irq);
}
TB_API double tb_gfx_scale(void) { return 1; }
TB_API char *tb_gfx_clip_get(void) {
    if (!tb_bclip) return 0;
    size_t n = strlen(tb_bclip);
    char *r = malloc(n + 1);
    if (r) memcpy(r, tb_bclip, n + 1);
    return r;
}
TB_API void tb_gfx_clip_set(const char *s) {
    size_t n = strlen(s);
    char *c = malloc(n + 1);
    if (!c) return;
    memcpy(c, s, n + 1);
    free(tb_bclip);
    tb_bclip = c;
}
TB_API void tb_gfx_cursor(int k) { tb_bcur = k; }
TB_API void tb_gfx_resizable(void) {}
TB_API void tb_gfx_reimage(void) {}
TB_API void tb_gpu_flush(int present) { (void)present; }
TB_API void tb_gpu_vsync(void) {}
TB_API int tb_gpu_resize(int ow, int oh) { (void)ow; (void)oh; return 1; }
// несколько окон (window_new): экран без ОС один, второе окно не открыть
#define TB_WPLAT int bare_pad;
#define TB_WPLAT_SAVE(s) (void)(s);
#define TB_WPLAT_LOAD(s) (void)(s);
TB_API uintptr_t tb_gfx_handle(void) { return 1; }
TB_API int tb_gfx_open2(const char *title) { (void)title; return 0; }
TB_API void tb_gfx_show(void) {}
TB_API void tb_gfx_close2(void) {}
TB_API void tb_gfx_poll(void) {}
TB_API void tb_gfx_fullscreen(int on) { (void)on; }   // экран без ОС и так целиком
TB_API void tb_gfx_lock(int on) { (void)on; }
TB_API int tb_gfx_open(const char *title) {
    (void)title;
    uint64_t lfb = tb_bga_find();
    if (!lfb || tb_bga_r(0) < 0xB0C0) return 0;
    tb_bga_w(4, 0);
    tb_bga_w(1, (unsigned short)tb_gw); tb_bga_w(2, (unsigned short)tb_gh); tb_bga_w(3, 32);
    tb_bga_w(6, (unsigned short)tb_gw); tb_bga_w(7, (unsigned short)tb_gh); tb_bga_w(8, 0); tb_bga_w(9, 0);
    tb_bga_w(4, 0x41);                             // включить, буфер кадра целиком
    if (tb_bga_r(1) != tb_gw || tb_bga_r(2) != tb_gh) return 0;
    tb_map_dev(lfb, (uint64_t)tb_gw * tb_gh * 4);
    tb_lfb = (volatile uint32_t *)(uintptr_t)lfb;
    tb_mx = tb_gw / 2; tb_my = tb_gh / 2;
    tb_key_hook = tb_gfx_key;
    tb_mouse_init();
    tb_bcur = tb_cursor_want;
    return 1;
}
// стрелка 12×19: 1 — контур (чёрный), 2 — заливка (белый)
TB_API const char *const tb_arrow[19] = {
    "1", "11", "121", "1221", "12221", "122221", "1222221", "12222221", "122222221", "1222222221", "12222222221", "122222211111", "1222121", "12201221", "12001221", "1000012210", "0000012210", "0000001221", "00000011" };
TB_API void tb_gfx_events(void) {
    if (!tb_lfb) return;
    uint64_t f = tb_irq_save();
    int mx = tb_mx, my = tb_my;
    tb_irq_restore(f);
    memcpy((void *)tb_lfb, tb_px, (size_t)tb_gw * tb_gh * 4);
    if (tb_bcur == 7) return;
    for (int y = 0; y < 19; y++)
        for (int x = 0; tb_arrow[y][x]; x++) {
            char c = tb_arrow[y][x];
            if (c == '0' || mx + x >= tb_gw || my + y >= tb_gh) continue;
            tb_lfb[(my + y) * tb_gw + mx + x] = c == '1' ? 0x000000 : 0xFFFFFF;
        }
}
