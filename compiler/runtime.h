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
//@include omnium/kernel/kernel.h
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
#ifdef TB_BARE
    if (tb_cell_can_fall()) tb_cell_fall(msg, line);
#endif
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
// большие и маленькие буквы: латиница и русские (русская буква в UTF-8 — 2 байта, длина та же)
TB_API const char *tb_upper(const char *s) {
    size_t n = strlen(s); unsigned char *r = (unsigned char *)tb_tmem(n + 1);
    memcpy(r, s, n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char a = r[i], b = r[i + 1];
        if (a >= 'a' && a <= 'z') r[i] = a - 32;
        else if (a == 0xD0 && b >= 0xB0 && b <= 0xBF) { r[i + 1] = b - 0x20; i++; }              // а..п → А..П
        else if (a == 0xD1 && b >= 0x80 && b <= 0x8F) { r[i] = 0xD0; r[i + 1] = b + 0x20; i++; } // р..я → Р..Я
        else if (a == 0xD1 && b == 0x91) { r[i] = 0xD0; r[i + 1] = 0x81; i++; }                  // ё → Ё
        else if (a >= 0xC0 && b) i++;
    }
    return (const char *)r;
}
TB_API const char *tb_lower(const char *s) {
    size_t n = strlen(s); unsigned char *r = (unsigned char *)tb_tmem(n + 1);
    memcpy(r, s, n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char a = r[i], b = r[i + 1];
        if (a >= 'A' && a <= 'Z') r[i] = a + 32;
        else if (a == 0xD0 && b >= 0x90 && b <= 0x9F) { r[i + 1] = b + 0x20; i++; }              // А..П → а..п
        else if (a == 0xD0 && b >= 0xA0 && b <= 0xAF) { r[i] = 0xD1; r[i + 1] = b - 0x20; i++; } // Р..Я → р..я
        else if (a == 0xD0 && b == 0x81) { r[i] = 0xD1; r[i + 1] = 0x91; i++; }                  // Ё → ё
        else if (a >= 0xC0 && b) i++;
    }
    return (const char *)r;
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
TB_API long long tb_time_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
TB_API long long tb_time_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
TB_API void tb_wait_ms(long long n) { fflush(stdout); if (n > 0) usleep((useconds_t)(n * 1000)); }
#endif
TB_API unsigned long long tb_rand_state = 0;
// цвет из красного, зелёного и синего (0..255) — одно число 0xRRGGBB
TB_API long long tb_rgb(long long r, long long g, long long b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (r << 16) | (g << 8) | b;
}
// цвет с непрозрачностью a: 255 — сплошной (как rgb), 0 — не виден. Рисование смешивает его с тем, что под ним
TB_API long long tb_rgba(long long r, long long g, long long b, long long a) {
    a = a < 0 ? 0 : a > 255 ? 255 : a;
    return ((255 - a) << 24) | tb_rgb(r, g, b);
}
// случайное целое от a до b (оба входят)
TB_API long long tb_random(long long a, long long b) {
    if (tb_rand_state == 0) {
#ifdef TB_BARE
        unsigned lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        tb_rand_state = ((unsigned long long)hi << 32 | lo) ^ 0x9E3779B97F4A7C15ULL;
#else
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        tb_rand_state = ((unsigned long long)ts.tv_nsec << 20) ^ (unsigned long long)ts.tv_sec ^ 0x9E3779B97F4A7C15ULL;
#endif
    }
    if (b < a) { long long t = a; a = b; b = t; }
    tb_rand_state ^= tb_rand_state << 13; tb_rand_state ^= tb_rand_state >> 7; tb_rand_state ^= tb_rand_state << 17;
    unsigned long long span = (unsigned long long)(b - a) + 1;
    return span == 0 ? (long long)tb_rand_state : a + (long long)(tb_rand_state % span);
}
#ifdef TB_BARE
TB_API const char *tb_env(const char *name) { (void)name; return ""; }   // переменных окружения в ядре нет
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
// a.crashed: в обычной программе падение задачи роняет всю программу — сюда доходят только целые
TB_API _Bool tb_crashed(tb_task *t) { tb_wait(t); return 0; }
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
// shared x += n без замка. В ядре без ОС одно ядро процессора: хватает выключить прерывания
#ifdef TB_BARE
static inline void tb_atomic_addi(volatile long long *p, long long v) { uint64_t f = tb_irq_save(); *p += v; tb_irq_restore(f); }
static inline void tb_atomic_addd(volatile double *p, double v) { uint64_t f = tb_irq_save(); *p += v; tb_irq_restore(f); }
#else
static inline void tb_atomic_addi(volatile long long *p, long long v) { __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
// дробь: сравнить и заменить, пока никто не успел поменять
static inline void tb_atomic_addd(volatile double *p, double v) {
    volatile uint64_t *q = (volatile uint64_t *)p;
    uint64_t old = __atomic_load_n(q, __ATOMIC_SEQ_CST), nw;
    do {
        union { uint64_t u; double d; } a, b;
        a.u = old; b.d = a.d + v; nw = b.u;
    } while (!__atomic_compare_exchange_n(q, &old, nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
}
#endif

// ---------- память руками: new_mem, free_mem, grow_mem, copy_mem, fill_mem (только в danger) ----------
// Перед каждым блоком 16 байт: размер и метка «живой» / «освобождён» — так free_mem ловит чужой
// адрес и второе освобождение. TB_MEM_CHECK=1 (не в ядре): за блоком 16 байт-сторожей, освобождённые
// блоки не отдаются обратно (их чтение и запись ловятся), read/write/copy_mem/fill_mem проверяют
// адрес, а в конце программы печатается, что не освобождено
#define TB_MEM_LIVE 0x4D454D4C49564521ULL
#define TB_MEM_DEAD 0x4D454D4445414421ULL
#define TB_MEM_GUARD 16
typedef struct { uint64_t size, magic; } tb_memh;
typedef struct { uintptr_t a; uint64_t n; int live, line, fline; } tb_memb;
TB_API int tb_mem_check = -1;
TB_API tb_memb *tb_mem_all = 0;
TB_API long long tb_mem_n = 0, tb_mem_cap = 0;
TB_API volatile int tb_mem_spin = 0;
TB_API void tb_mem_report(void);
TB_INL int tb_memchk(void) {
    if (TB_RARE(tb_mem_check < 0)) {
#ifdef TB_BARE
        tb_mem_check = 0;
#else
        const char *e = getenv("TB_MEM_CHECK");
        tb_mem_check = e && e[0] == '1';
        if (tb_mem_check) atexit(tb_mem_report);
#endif
    }
    return tb_mem_check;
}
static void tb_mem_lock(void) { while (__sync_lock_test_and_set(&tb_mem_spin, 1)) { } }
static void tb_mem_unlock(void) { __sync_lock_release(&tb_mem_spin); }
TB_COLD static void tb_mem_die(int line, const char *what, int at) {
    char buf[200];
    if (at > 0) snprintf(buf, sizeof buf, "%s (блок из new_mem в строке %d)", what, at);
    else snprintf(buf, sizeof buf, "%s", what);
    tb_mem_check = 0;   // программа падает: отчёт о неосвобождённом не нужен
    tb_mem_unlock();
    tb_crash(line, buf);
}
TB_API void tb_mem_report(void) {
    long long n = 0, bytes = 0; int first = 0;
    for (long long i = 0; i < tb_mem_n; i++)
        if (tb_mem_all[i].live) { if (!n) first = tb_mem_all[i].line; n++; bytes += (long long)tb_mem_all[i].n; }
    if (n && tb_mem_check) fprintf(stderr, "TB_MEM_CHECK: не освобождено блоков %lld (%lld байт), первый из new_mem в строке %d\n", n, bytes, first);
}
// TB_MEM_CHECK: можно ли трогать байты [a, a + n)
static void tb_mem_touch(uintptr_t a, uint64_t n, int line) {
    if (n == 0) return;
    tb_mem_lock();
    for (long long i = 0; i < tb_mem_n; i++) {
        tb_memb *b = &tb_mem_all[i];
        uintptr_t lo = b->a - sizeof(tb_memh), hi = b->a + b->n + TB_MEM_GUARD;
        if (a + n <= lo || a >= hi) continue;
        if (!b->live) {
            char buf[160];
            snprintf(buf, sizeof buf, "эта память уже освобождена: free_mem в строке %d", b->fline);
            tb_mem_die(line, buf, b->line);
        }
        if (a < b->a || a + n > b->a + b->n) tb_mem_die(line, "чтение или запись за краем блока", b->line);
    }
    tb_mem_unlock();
}
static tb_memb *tb_mem_find(uintptr_t a) {
    for (long long i = tb_mem_n - 1; i >= 0; i--) if (tb_mem_all[i].a == a) return &tb_mem_all[i];
    return 0;
}
// чтение и запись по адресу: volatile — компилятор C не выкинет и не переставит их
#define TB_TOUCH(a, n) if (TB_RARE(tb_memchk())) tb_mem_touch((uintptr_t)(a), (n), line)
TB_INL long long tb_rd8(long long a, int line) { TB_TOUCH(a, 1); return *(volatile uint8_t *)(uintptr_t)a; }
TB_INL long long tb_rd16(long long a, int line) { TB_TOUCH(a, 2); return *(volatile uint16_t *)(uintptr_t)a; }
TB_INL long long tb_rd32(long long a, int line) { TB_TOUCH(a, 4); return *(volatile uint32_t *)(uintptr_t)a; }
TB_INL long long tb_rd64(long long a, int line) { TB_TOUCH(a, 8); return (long long)*(volatile uint64_t *)(uintptr_t)a; }
TB_INL void tb_wr8(long long a, long long v, int line) { TB_TOUCH(a, 1); *(volatile uint8_t *)(uintptr_t)a = (uint8_t)v; }
TB_INL void tb_wr16(long long a, long long v, int line) { TB_TOUCH(a, 2); *(volatile uint16_t *)(uintptr_t)a = (uint16_t)v; }
TB_INL void tb_wr32(long long a, long long v, int line) { TB_TOUCH(a, 4); *(volatile uint32_t *)(uintptr_t)a = (uint32_t)v; }
TB_INL void tb_wr64(long long a, long long v, int line) { TB_TOUCH(a, 8); *(volatile uint64_t *)(uintptr_t)a = (uint64_t)v; }
TB_INL long long tb_addr_of(tb_list *l) { return (long long)(uintptr_t)l->data; }
// new_mem(байт): новый блок, заполнен нулями; адрес — число. Может не получиться (нет памяти)
TB_API long long tb_new_mem(long long n, int line) {
    if (n <= 0) { tb_err = "new_mem: размер должен быть больше нуля"; return 0; }
    if ((unsigned long long)n > ((unsigned long long)1 << 46)) { tb_err = "new_mem: слишком большой блок"; return 0; }
    int chk = tb_memchk();
    tb_memh *h = malloc(sizeof(tb_memh) + (size_t)n + (chk ? TB_MEM_GUARD : 0));
    if (!h) { tb_err = "new_mem: не хватило памяти"; return 0; }
    h->size = (uint64_t)n; h->magic = TB_MEM_LIVE;
    unsigned char *p = (unsigned char *)(h + 1);
    memset(p, 0, (size_t)n);
    if (chk) {
        memset(p + n, 0xFB, TB_MEM_GUARD);
        tb_mem_lock();
        if (tb_mem_n == tb_mem_cap) {
            long long nc = tb_mem_cap ? tb_mem_cap * 2 : 256;
            tb_memb *all = realloc(tb_mem_all, (size_t)nc * sizeof(tb_memb));
            if (!all) tb_mem_die(line, "не хватило памяти", 0);
            tb_mem_all = all; tb_mem_cap = nc;
        }
        tb_memb b = { (uintptr_t)p, (uint64_t)n, 1, line, 0 };
        tb_mem_all[tb_mem_n++] = b;
        tb_mem_unlock();
    }
    return (long long)(uintptr_t)p;
}
// проверить адрес для free_mem / grow_mem; вернуть заголовок блока
static tb_memh *tb_mem_own(long long a, int line, const char *who) {
    char buf[120];
    if (tb_memchk()) {
        tb_mem_lock();
        tb_memb *b = tb_mem_find((uintptr_t)a);
        if (!b) { snprintf(buf, sizeof buf, "%s: это не адрес из new_mem", who); tb_mem_die(line, buf, 0); }
        if (!b->live) {
            snprintf(buf, sizeof buf, "%s: блок уже освобождён в строке %d", who, b->fline);
            tb_mem_die(line, buf, b->line);
        }
        const unsigned char *g = (const unsigned char *)(uintptr_t)a + b->n;
        for (int i = 0; i < TB_MEM_GUARD; i++)
            if (g[i] != 0xFB) tb_mem_die(line, "кто-то писал за краем блока", b->line);
        tb_mem_unlock();
        return (tb_memh *)(uintptr_t)a - 1;
    }
    tb_memh *h = (tb_memh *)(uintptr_t)a - 1;
    if (h->magic == TB_MEM_DEAD) { snprintf(buf, sizeof buf, "%s: блок уже освобождён", who); tb_crash(line, buf); }
    if (h->magic != TB_MEM_LIVE) { snprintf(buf, sizeof buf, "%s: это не адрес из new_mem или блок уже освобождён (точнее покажет TB_MEM_CHECK=1)", who); tb_crash(line, buf); }
    return h;
}
// TB_MEM_CHECK: блок не отдаётся обратно, а помечается — чтобы поймать чтение после free_mem
static void tb_mem_bury(long long a, int line) {
    tb_mem_lock();
    tb_memb *b = tb_mem_find((uintptr_t)a);
    b->live = 0; b->fline = line;
    memset((void *)(uintptr_t)a, 0xDD, (size_t)b->n);
    ((tb_memh *)(uintptr_t)a - 1)->magic = TB_MEM_DEAD;
    tb_mem_unlock();
}
// free_mem(адрес): отдать блок; free_mem(0) ничего не делает
TB_API void tb_free_mem(long long a, int line) {
    if (a == 0) return;
    tb_memh *h = tb_mem_own(a, line, "free_mem");
    if (tb_memchk()) { tb_mem_bury(a, line); return; }
    h->magic = TB_MEM_DEAD;
    free(h);
}
// grow_mem(адрес, байт): блок нового размера (данные сохраняются, новое место — нули); адрес может стать другим
TB_API long long tb_grow_mem(long long a, long long n, int line) {
    if (a == 0) return tb_new_mem(n, line);
    if (n <= 0) { tb_err = "grow_mem: размер должен быть больше нуля"; return 0; }
    if ((unsigned long long)n > ((unsigned long long)1 << 46)) { tb_err = "grow_mem: слишком большой блок"; return 0; }
    tb_memh *h = tb_mem_own(a, line, "grow_mem");
    uint64_t old = h->size;
    if (tb_memchk()) {
        long long b = tb_new_mem(n, line);
        if (!b) return 0;
        memcpy((void *)(uintptr_t)b, (void *)(uintptr_t)a, (size_t)(old < (uint64_t)n ? old : (uint64_t)n));
        tb_mem_bury(a, line);
        return b;
    }
    tb_memh *q = realloc(h, sizeof(tb_memh) + (size_t)n);
    if (!q) { tb_err = "grow_mem: не хватило памяти"; return 0; }
    if ((uint64_t)n > old) memset((unsigned char *)(q + 1) + old, 0, (size_t)((uint64_t)n - old));
    q->size = (uint64_t)n;
    return (long long)(uintptr_t)(q + 1);
}
// copy_mem(куда, откуда, байт): блоки могут перекрываться
TB_API void tb_copy_mem(long long dst, long long src, long long n, int line) {
    if (n < 0) tb_crash(line, "copy_mem: число байт меньше нуля");
    if (n == 0) return;
    if (tb_memchk()) { tb_mem_touch((uintptr_t)src, (uint64_t)n, line); tb_mem_touch((uintptr_t)dst, (uint64_t)n, line); }
    memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n);
}
// ---------- use c: функции из C ----------
// настоящее имя функции C (на Mac у имён в библиотеке спереди _)
#ifdef __APPLE__
#define TB_CSYM(s) __asm__("_" s)
#else
#define TB_CSYM(s) __asm__(s)
#endif
// текст из C: копия в память Tachbit (C может потом поменять или отдать свой); NULL → ""
TB_API const char *tb_ctext(const char *p) {
    if (!p) return "";
    size_t n = strlen(p);
    char *s = tb_tmem(n + 1);
    memcpy(s, p, n + 1);
    return s;
}

// ---------- zone: область памяти. Много кусков берутся быстро и освобождаются разом ----------
// Зона — цепочка больших кусков. zone_take отрезает от текущего куска (16 байт выравнивание, нули),
// кончился кусок — берёт новый. zone_clear отдаёт всё, кроме первого куска; zone_free — всё.
#define TB_ZONE_LIVE 0x5A4F4E454C495645ULL
typedef struct tb_zchunk { struct tb_zchunk *next; uint64_t size, used, pad; } tb_zchunk;   // 32 байта: данные с выравниванием 16
typedef struct { uint64_t magic, step; tb_zchunk *first, *cur; } tb_zone;
// данные куска: сразу за заголовком, с выравниванием 16 (malloc в ядре выравнивает меньше)
#define TB_ZDATA(k) ((unsigned char *)(((uintptr_t)((k) + 1) + 15) & ~(uintptr_t)15))
static tb_zchunk *tb_zone_chunk(uint64_t n) {
    tb_zchunk *k = malloc(sizeof(tb_zchunk) + (size_t)n + 16);
    if (!k) return 0;
    k->next = 0; k->size = n; k->used = 0;
    memset(TB_ZDATA(k), 0, (size_t)n);
    return k;
}
// zone_new(байт): первый кусок такого размера; дальше куски того же размера (или больше, если нужно)
TB_API long long tb_zone_new(long long n, int line) {
    (void)line;
    if (n <= 0) { tb_err = "zone_new: размер должен быть больше нуля"; return 0; }
    if ((unsigned long long)n > ((unsigned long long)1 << 46)) { tb_err = "zone_new: слишком большой размер"; return 0; }
    tb_zone *z = malloc(sizeof(tb_zone));
    if (!z) { tb_err = "zone_new: не хватило памяти"; return 0; }
    uint64_t step = ((uint64_t)n + 15) & ~(uint64_t)15;
    z->first = z->cur = tb_zone_chunk(step);
    if (!z->first) { free(z); tb_err = "zone_new: не хватило памяти"; return 0; }
    z->magic = TB_ZONE_LIVE; z->step = step;
    return (long long)(uintptr_t)z;
}
static tb_zone *tb_zone_own(long long a, int line, const char *who) {
    tb_zone *z = (tb_zone *)(uintptr_t)a;
    if (!z || z->magic != TB_ZONE_LIVE) {
        char buf[120];
        snprintf(buf, sizeof buf, "%s: это не зона из zone_new или она уже освобождена", who);
        tb_crash(line, buf);
    }
    return z;
}
// zone_take(зона, байт): адрес нового куска (нули)
TB_API long long tb_zone_take(long long a, long long n, int line) {
    tb_zone *z = tb_zone_own(a, line, "zone_take");
    if (n <= 0) tb_crash(line, "zone_take: размер должен быть больше нуля");
    uint64_t need = ((uint64_t)n + 15) & ~(uint64_t)15;
    tb_zchunk *k = z->cur;
    if (k->size - k->used < need) {
        if (k->next && k->next->size >= need) k = k->next;
        else {
            tb_zchunk *nk = tb_zone_chunk(need > z->step ? need : z->step);
            if (!nk) tb_crash(line, "zone_take: не хватило памяти");
            nk->next = k->next; k->next = nk; k = nk;
        }
        z->cur = k;
    }
    unsigned char *p = TB_ZDATA(k) + k->used;
    k->used += need;
    return (long long)(uintptr_t)p;
}
// zone_clear(зона): всё взятое больше не нужно; первый кусок остаётся для новых zone_take
TB_API void tb_zone_clear(long long a, int line) {
    tb_zone *z = tb_zone_own(a, line, "zone_clear");
    tb_zchunk *k = z->first->next;
    while (k) { tb_zchunk *nx = k->next; free(k); k = nx; }
    z->first->next = 0;
    memset(TB_ZDATA(z->first), 0, (size_t)z->first->used);
    z->first->used = 0;
    z->cur = z->first;
}
// zone_free(зона): отдать всё; zone_free(0) ничего не делает
TB_API void tb_zone_free(long long a, int line) {
    if (a == 0) return;
    tb_zone *z = tb_zone_own(a, line, "zone_free");
    tb_zchunk *k = z->first;
    while (k) { tb_zchunk *nx = k->next; free(k); k = nx; }
    z->magic = 0;
    free(z);
}
// zone_used(зона): сколько байт взято (с выравниванием)
TB_API long long tb_zone_used(long long a, int line) {
    tb_zone *z = tb_zone_own(a, line, "zone_used");
    long long s = 0;
    for (tb_zchunk *k = z->first; k; k = k->next) s += (long long)k->used;
    return s;
}

// ---------- syscall(номер, ...): прямой вызов ядра ОС (Linux, Mac). Ответ -1 → -номер ошибки ----------
#if !defined(TB_BARE) && !defined(_WIN32)
#include <sys/syscall.h>
#include <errno.h>
#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
TB_API long long tb_syscall(long long n, long long a, long long b, long long c, long long d, long long e, long long f, int line) {
    (void)line;
    long r = syscall((long)n, (long)a, (long)b, (long)c, (long)d, (long)e, (long)f);
    if (r == -1) return -(long long)errno;
    return (long long)r;
}
#ifdef __APPLE__
#pragma clang diagnostic pop
#endif
#else
TB_API long long tb_syscall(long long n, long long a, long long b, long long c, long long d, long long e, long long f, int line) {
    (void)n; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    tb_crash(line, "syscall есть только на Linux и Mac");
    return 0;
}
#endif

// ---------- shape: поля с размером прямо в памяти (регистры, заголовки пакетов) ----------
// k = смещение × 2^20 + байт (1, 2, 4, 8; 0 — bytes: отдать адрес) + 16 со знаком + 32 big
//     + 64 × первый бит + 4096 × сколько бит (0 — поле целиком).
// Поле читается и пишется одним volatile-доступом своей ширины: регистр u32 — одной 32-битной операцией.
static inline uint64_t tb_shape_raw(uintptr_t p, int nb) {
    if (nb == 1) return *(volatile uint8_t *)p;
    if (nb == 2) return *(volatile uint16_t *)p;
    if (nb == 4) return *(volatile uint32_t *)p;
    return *(volatile uint64_t *)p;
}
static inline void tb_shape_put(uintptr_t p, int nb, uint64_t v) {
    if (nb == 1) *(volatile uint8_t *)p = (uint8_t)v;
    else if (nb == 2) *(volatile uint16_t *)p = (uint16_t)v;
    else if (nb == 4) *(volatile uint32_t *)p = (uint32_t)v;
    else *(volatile uint64_t *)p = v;
}
static inline uint64_t tb_shape_swap(uint64_t v, int nb) {
    if (nb == 2) return __builtin_bswap16((uint16_t)v);
    if (nb == 4) return __builtin_bswap32((uint32_t)v);
    if (nb == 8) return __builtin_bswap64(v);
    return v;
}
TB_INL long long tb_shape_at(long long a) { return a; }
TB_API long long tb_shape_get(long long a, long long k, int line) {
    uintptr_t p = (uintptr_t)(a + (k >> 20));
    int nb = (int)(k & 15), bo = (int)((k >> 6) & 63), bn = (int)((k >> 12) & 127);
    if (!nb) return (long long)p;
    TB_TOUCH(p, nb);
    uint64_t v = tb_shape_raw(p, nb);
    if (k & 32) v = tb_shape_swap(v, nb);
    if (bn) {
        v >>= bo;
        if (bn < 64) v &= ((uint64_t)1 << bn) - 1;
        return (long long)v;
    }
    if (k & 16) {
        if (nb < 8) { int sh = 64 - 8 * nb; return (long long)(v << sh) >> sh; }
        return (long long)v;
    }
    if (nb == 8 && (v >> 63)) tb_crash(line, "в поле u64 число больше, чем влезает в int");
    return (long long)v;
}
TB_API void tb_shape_set(long long a, long long k, long long x, int line) {
    uintptr_t p = (uintptr_t)(a + (k >> 20));
    int nb = (int)(k & 15), bo = (int)((k >> 6) & 63), bn = (int)((k >> 12) & 127);
    static const char *uns[9] = { 0, "число не влезает в поле u8: от 0 до 255", "число не влезает в поле u16: от 0 до 65 535", 0,
        "число не влезает в поле u32: от 0 до 4 294 967 295", 0, 0, 0, "в поле u64 не бывает чисел меньше нуля" };
    static const char *sig[9] = { 0, "число не влезает в поле i8: от -128 до 127", "число не влезает в поле i16: от -32 768 до 32 767", 0,
        "число не влезает в поле i32: от -2 147 483 648 до 2 147 483 647", 0, 0, 0, 0 };
    TB_TOUCH(p, nb);
    if (bn) {
        if (x < 0 || (bn < 63 && (x >> bn))) tb_crash(line, "число не влезает в поле bits: от 0 до 2 в степени N без единицы");
        uint64_t m = bn < 64 ? (((uint64_t)1 << bn) - 1) << bo : ~(uint64_t)0;
        uint64_t v = tb_shape_raw(p, nb);
        if (k & 32) v = tb_shape_swap(v, nb);
        v = (v & ~m) | (((uint64_t)x << bo) & m);
        if (k & 32) v = tb_shape_swap(v, nb);
        tb_shape_put(p, nb, v);
        return;
    }
    if (k & 16) {
        if (nb < 8) { long long hi = ((long long)1 << (8 * nb - 1)) - 1; if (x > hi || x < -hi - 1) tb_crash(line, sig[nb]); }
    } else if (x < 0 || (nb < 8 && (x >> (8 * nb)))) tb_crash(line, uns[nb]);
    uint64_t v = (uint64_t)x;
    if (k & 32) v = tb_shape_swap(v, nb);
    tb_shape_put(p, nb, v);
}
// fill_mem(адрес, байт-значение, сколько)
TB_API void tb_fill_mem(long long a, long long v, long long n, int line) {
    if (n < 0) tb_crash(line, "fill_mem: число байт меньше нуля");
    if (n == 0) return;
    if (tb_memchk()) tb_mem_touch((uintptr_t)a, (uint64_t)n, line);
    memset((void *)(uintptr_t)a, (int)(v & 255), (size_t)n);
}
#ifdef TB_BARE
TB_INL long long tb_pin8(long long p) { return tb_inb((unsigned short)p); }
TB_INL long long tb_pin16(long long p) { return tb_inw((unsigned short)p); }
TB_INL long long tb_pin32(long long p) { return tb_inl((unsigned short)p); }
TB_INL void tb_pout8(long long p, long long v) { tb_outb((unsigned short)p, (unsigned char)v); }
TB_INL void tb_pout16(long long p, long long v) { tb_outw((unsigned short)p, (unsigned short)v); }
TB_INL void tb_pout32(long long p, long long v) { tb_outl((unsigned short)p, (unsigned int)v); }
// замок в ядре: одна команда xchg + прерывания выключены, пока замок у нас
// (так прерывание не застрянет на замке, который держит прерванная задача)
// Клетка помнит, какие замки держит: упадёт — ядро их отпустит
TB_INL unsigned long tb_lock_take(int *l) {
    uint64_t f = tb_irq_save();
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) { tb_irq_restore(f); tb_yield(); f = tb_irq_save(); }
    tb_kslot *s = &tb_slots[tb_cur];
    if (!tb_in_irq && s->nlocks < 8) s->locks[s->nlocks++] = l;
    return f;
}
TB_INL void tb_lock_give(int *l, unsigned long f) {
    tb_kslot *s = &tb_slots[tb_cur];
    if (!tb_in_irq)
        for (int i = s->nlocks - 1; i >= 0; i--)
            if (s->locks[i] == l) { s->locks[i] = s->locks[--s->nlocks]; break; }
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
    tb_irq_restore((uint64_t)f);
}
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
// ch.ready(): в канале уже лежит значение — receive() не будет ждать
TB_API _Bool tb_chan_ready(tb_chan *ch) {
    TB_CH_LOCK(ch);
    int r = ch->len > 0;
    TB_CH_UNLOCK(ch);
    return r;
}
#define TB_CH_RECV(T, ch, line) ({ T tb_cv; if (!tb_chan_take((ch), &tb_cv)) tb_crash((line), "канал закрыт и пуст: receive() нечего взять"); tb_cv; })

// ---------- окно и рисование (только если программа рисует) ----------
#if defined(TB_GFX)
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
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xfe,0x00},
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
// короткий щелчок (касание тачпада): нажали и отпустили между двумя кадрами — mouse_down() всё равно разок скажет «да»
TB_API int tb_mtap = 0;
// колёсико: шагов с прошлого mouse_wheel() (+ вперёд, от себя; − назад); у тачпада шаги дробные — копим
TB_API int tb_wheel = 0;
// боковые кнопки мыши: [0] назад, [1] вперёд; tap — нажали и отпустили между кадрами
TB_API int tb_mside[2], tb_mside_tap[2];
TB_API void tb_side(int b, int down) { tb_mside[b] = down; if (down) tb_mside_tap[b] = 1; }
TB_API double tb_wheel_f = 0;
TB_API void tb_wheel_add(double d) {
    tb_wheel_f += d;
    int n = (int)tb_wheel_f;
    tb_wheel += n;
    tb_wheel_f -= n;
}
// буквы, набранные с прошлого typed_text(), в UTF-8 (стрелки, Enter и прочие не буквы не попадают)
TB_API char tb_typed[512];
TB_API int tb_ntyped = 0;
TB_API char *tb_gfx_clip_get(void);          // свои у каждой системы: текст (malloc) или 0
TB_API void tb_gfx_clip_set(const char *s);
TB_API void tb_gfx_cursor(int k);              // 0 стрелка, 1 рука, 2 текст, 3 двигать, 4 ↔, 5 ↕, 6 ждать, 7 спрятать
TB_API int tb_cursor_want = 0;                 // курсор, который просили до window_open
TB_API void tb_type_cp(unsigned u) {
    if (u < 32 || u == 127 || (u >= 0xF700 && u <= 0xF8FF) || u > 0x10FFFF) return;
    if (tb_ntyped + 4 >= (int)sizeof tb_typed) return;
    char *o = tb_typed + tb_ntyped;
    if (u < 0x80) { o[0] = (char)u; tb_ntyped += 1; }
    else if (u < 0x800) { o[0] = (char)(0xC0 | (u >> 6)); o[1] = (char)(0x80 | (u & 63)); tb_ntyped += 2; }
    else if (u < 0x10000) { o[0] = (char)(0xE0 | (u >> 12)); o[1] = (char)(0x80 | ((u >> 6) & 63)); o[2] = (char)(0x80 | (u & 63)); tb_ntyped += 3; }
    else { o[0] = (char)(0xF0 | (u >> 18)); o[1] = (char)(0x80 | ((u >> 12) & 63)); o[2] = (char)(0x80 | ((u >> 6) & 63)); o[3] = (char)(0x80 | (u & 63)); tb_ntyped += 4; }
}
TB_API void tb_type_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (p && *p) {
        unsigned u = *p++;
        if (u >= 0x80) {
            int more = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : u >= 0xC0 ? 1 : 0;
            u &= 0x3F >> more;
            while (more-- > 0 && (*p & 0xC0) == 0x80) u = (u << 6) | (*p++ & 0x3F);
        }
        tb_type_cp(u);
    }
}
TB_API unsigned char tb_keys[256];
TB_API unsigned char tb_ktap[256];   // клавишу нажали и отпустили между кадрами: key_down() разок скажет «да»
TB_API long long tb_last_frame = 0;
// клавиши: буквы и цифры — свой код ASCII (A..Z, 0..9), остальные:
enum { TB_K_ENTER = 10, TB_K_ESC = 27, TB_K_SPACE = 32, TB_K_BACK = 8, TB_K_TAB = 9,
       TB_K_UP = 17, TB_K_DOWN = 18, TB_K_RIGHT = 19, TB_K_LEFT = 20, TB_K_SHIFT = 21,
       TB_K_CMD = 1, TB_K_CTRL = 2, TB_K_ALT = 3, TB_K_DEL = 4, TB_K_HOME = 5, TB_K_END = 6, TB_K_PGUP = 7,
       TB_K_PGDN = 11, TB_K_F1 = 128 };   // F1..F12 — 128..139
// ---------- картинки PNG (load_image) ----------
// Свой маленький разбор PNG: распаковка deflate (как zlib) и фильтры строк.
// Картинка хранится точками 0xAARRGGBB; точка видна, если её прозрачность (A) не меньше 128.
typedef struct { const unsigned char *p, *end; unsigned bits; int nb, err; unsigned char *out; size_t n, cap; } tb_zs;
typedef struct { short count[16], sym[320]; } tb_huff;
TB_API unsigned tb_zbits(tb_zs *z, int n) {
    while (z->nb < n) {
        if (z->p >= z->end) { z->err = 1; return 0; }
        z->bits |= (unsigned)*z->p++ << z->nb; z->nb += 8;
    }
    unsigned v = z->bits & ((1u << n) - 1);
    z->bits >>= n; z->nb -= n;
    return v;
}
TB_API void tb_zput(tb_zs *z, unsigned char b) {
    if (z->n == z->cap) {
        size_t nc = z->cap ? z->cap * 2 : 65536;
        unsigned char *q = realloc(z->out, nc);
        if (!q) { z->err = 1; return; }
        z->out = q; z->cap = nc;
    }
    z->out[z->n++] = b;
}
TB_API void tb_hbuild(tb_huff *h, const unsigned char *len, int n) {
    short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (short)i;
}
TB_API int tb_hdec(tb_zs *z, const tb_huff *h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)tb_zbits(z, 1);
        int c = h->count[l];
        if (code - c < first) return h->sym[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
        if (z->err) return -1;
    }
    z->err = 1;
    return -1;
}
TB_API int tb_inflate(tb_zs *z) {
    static const short lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const unsigned char lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const short dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const unsigned char dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
    static const unsigned char order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    int last;
    do {
        last = (int)tb_zbits(z, 1);
        int type = (int)tb_zbits(z, 2);
        if (type == 0) {   // без сжатия
            z->bits >>= z->nb & 7; z->nb -= z->nb & 7;
            unsigned len = tb_zbits(z, 16), nlen = tb_zbits(z, 16);
            if ((len ^ 0xFFFF) != nlen) return 0;
            while (len-- && !z->err) tb_zput(z, (unsigned char)tb_zbits(z, 8));
        } else if (type == 1 || type == 2) {
            tb_huff lh, dh;
            unsigned char lens[320];
            int nl = 288, nd = 30;
            if (type == 1) {   // готовые коды
                for (int i = 0; i < 144; i++) lens[i] = 8;
                for (int i = 144; i < 256; i++) lens[i] = 9;
                for (int i = 256; i < 280; i++) lens[i] = 7;
                for (int i = 280; i < 288; i++) lens[i] = 8;
                for (int i = 0; i < 30; i++) lens[288 + i] = 5;
            } else {           // свои коды в начале блока
                nl = (int)tb_zbits(z, 5) + 257; nd = (int)tb_zbits(z, 5) + 1;
                int nc = (int)tb_zbits(z, 4) + 4;
                unsigned char cl[19] = { 0 };
                for (int i = 0; i < nc; i++) cl[order[i]] = (unsigned char)tb_zbits(z, 3);
                tb_huff ch; tb_hbuild(&ch, cl, 19);
                int i = 0;
                while (i < nl + nd && !z->err) {
                    int s = tb_hdec(z, &ch), rep = 0, val = 0;
                    if (s < 16) { lens[i++] = (unsigned char)s; continue; }
                    if (s == 16) { if (!i) return 0; val = lens[i - 1]; rep = 3 + (int)tb_zbits(z, 2); }
                    else if (s == 17) rep = 3 + (int)tb_zbits(z, 3);
                    else if (s == 18) rep = 11 + (int)tb_zbits(z, 7);
                    else return 0;
                    if (i + rep > nl + nd) return 0;
                    while (rep--) lens[i++] = (unsigned char)val;
                }
                if (type == 2) memmove(lens + 288, lens + nl, (size_t)nd);
            }
            tb_hbuild(&lh, lens, nl);
            tb_hbuild(&dh, lens + 288, nd);
            while (!z->err) {
                int s = tb_hdec(z, &lh);
                if (s < 0) return 0;
                if (s < 256) { tb_zput(z, (unsigned char)s); continue; }
                if (s == 256) break;
                s -= 257;
                if (s >= 29) return 0;
                int len = lbase[s] + (int)tb_zbits(z, lext[s]);
                int d = tb_hdec(z, &dh);
                if (d < 0 || d >= 30) return 0;
                size_t dist = (size_t)dbase[d] + tb_zbits(z, dext[d]);
                if (dist > z->n) return 0;
                while (len-- && !z->err) tb_zput(z, z->out[z->n - dist]);
            }
        } else return 0;
    } while (!last && !z->err);
    return !z->err;
}
TB_API unsigned tb_be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }
TB_INL int tb_paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
// разобрать PNG из памяти; ответ — точки 0xAARRGGBB (malloc) или 0 и текст ошибки в *why
TB_API uint32_t *tb_png(const unsigned char *d, size_t n, int *pw, int *ph, const char **why) {
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    *why = "это не PNG";
    if (n < 33 || memcmp(d, sig, 8) != 0) return 0;
    unsigned w = 0, h = 0; int depth = 0, ctype = 0, inter = 0;
    unsigned char pal[256][4]; int npal = 0, trns = 0; unsigned key[3] = { 0, 0, 0 };
    memset(pal, 255, sizeof pal);
    unsigned char *idat = 0; size_t ni = 0, ci = 0;
    size_t at = 8;
    while (at + 12 <= n) {
        unsigned len = tb_be32(d + at);
        const unsigned char *type = d + at + 4, *c = d + at + 8;
        if (len > n - at - 12) { free(idat); *why = "файл PNG обрезан"; return 0; }
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = tb_be32(c); h = tb_be32(c + 4); depth = c[8]; ctype = c[9]; inter = c[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            npal = (int)(len / 3 > 256 ? 256 : len / 3);
            for (int i = 0; i < npal; i++) { pal[i][0] = c[i * 3]; pal[i][1] = c[i * 3 + 1]; pal[i][2] = c[i * 3 + 2]; }
        } else if (!memcmp(type, "tRNS", 4)) {
            trns = 1;
            if (ctype == 3) for (unsigned i = 0; i < len && i < 256; i++) pal[i][3] = c[i];
            else if (ctype == 0 && len >= 2) key[0] = ((unsigned)c[0] << 8) | c[1];
            else if (ctype == 2 && len >= 6) for (int k = 0; k < 3; k++) key[k] = ((unsigned)c[k * 2] << 8) | c[k * 2 + 1];
        } else if (!memcmp(type, "IDAT", 4)) {
            if (ni + len > ci) {
                ci = (ni + len) * 2;
                unsigned char *q = realloc(idat, ci);
                if (!q) { free(idat); *why = "не хватило памяти"; return 0; }
                idat = q;
            }
            memcpy(idat + ni, c, len); ni += len;
        } else if (!memcmp(type, "IEND", 4)) break;
        at += 12 + len;
    }
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!w || !h || w > 16384 || h > 16384 || !ch || !idat) { free(idat); *why = "в PNG нет картинки"; return 0; }
    if (inter) { free(idat); *why = "PNG с чересстрочностью (interlace) не умею: сохрани без неё"; return 0; }
    if (!(depth == 8 || depth == 16 || (depth < 8 && (ctype == 0 || ctype == 3) && (depth == 1 || depth == 2 || depth == 4)))) {
        free(idat); *why = "такую глубину цвета PNG не умею"; return 0;
    }
    tb_zs z; memset(&z, 0, sizeof z);
    z.p = idat + 2; z.end = idat + ni;   // 2 байта заголовка zlib
    int ok = ni > 2 && (idat[0] & 15) == 8 && tb_inflate(&z);
    free(idat);
    size_t bitsp = (size_t)ch * depth, stride = (w * bitsp + 7) / 8, bpp = bitsp >= 8 ? bitsp / 8 : 1;
    if (!ok || z.n < (stride + 1) * h) { free(z.out); *why = "PNG испорчен"; return 0; }
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) { free(z.out); *why = "не хватило памяти"; return 0; }
    unsigned char *prev = 0;
    for (unsigned y = 0; y < h; y++) {
        unsigned char *row = z.out + y * (stride + 1), f = row[0], *r = row + 1;
        for (size_t i = 0; i < stride; i++) {   // фильтры строк PNG
            int a = i >= bpp ? r[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0;
            int v = r[i];
            if (f == 1) v += a; else if (f == 2) v += b; else if (f == 3) v += (a + b) / 2; else if (f == 4) v += tb_paeth(a, b, c);
            r[i] = (unsigned char)v;
        }
        prev = r;
        for (unsigned x = 0; x < w; x++) {
            unsigned s[4] = { 0, 0, 0, 255 }, raw[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < ch; k++) {
                if (depth == 8) raw[k] = r[x * ch + k], s[k] = raw[k];
                else if (depth == 16) raw[k] = ((unsigned)r[(x * ch + k) * 2] << 8) | r[(x * ch + k) * 2 + 1], s[k] = raw[k] >> 8;
                else {
                    unsigned bit = x * depth, v = (r[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
                    raw[k] = v; s[k] = ctype == 3 ? v : v * 255 / ((1u << depth) - 1);
                }
            }
            unsigned R, G, B, A;
            if (ctype == 3) { unsigned i = s[0] < 256 ? s[0] : 0; R = pal[i][0]; G = pal[i][1]; B = pal[i][2]; A = pal[i][3]; }
            else if (ctype == 0 || ctype == 4) { R = G = B = s[0]; A = ctype == 4 ? s[1] : (trns && raw[0] == key[0] ? 0 : 255); }
            else { R = s[0]; G = s[1]; B = s[2]; A = ctype == 6 ? s[3] : (trns && raw[0] == key[0] && raw[1] == key[1] && raw[2] == key[2] ? 0 : 255); }
            px[(size_t)y * w + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
    free(z.out);
    *pw = (int)w; *ph = (int)h;
    return px;
}
// все картинки программы: точки подряд в одном большом массиве (его же видит видеокарта)
typedef struct { int w, h; uint32_t off; } tb_image;
TB_API tb_image *tb_imgs = 0;
TB_API int tb_nimg = 0, tb_img_dirty = 0;
TB_API uint32_t *tb_imgpx = 0;
TB_API size_t tb_imgpx_n = 0;
TB_API long long tb_load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { tb_err = "нет такого файла картинки"; return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = sz > 0 ? malloc((size_t)sz) : 0;
    if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); tb_err = "не могу прочитать файл картинки"; return -1; }
    fclose(f);
    int w = 0, h = 0; const char *why = 0;
    uint32_t *px = tb_png(d, (size_t)sz, &w, &h, &why);
    free(d);
    if (!px) { tb_err = why; return -1; }
    size_t need = tb_imgpx_n + (size_t)w * h;
    if (need > 0xFFFFFFFFu) { free(px); tb_err = "слишком много картинок"; return -1; }
    uint32_t *all = realloc(tb_imgpx, need * 4);
    tb_image *list = realloc(tb_imgs, (size_t)(tb_nimg + 1) * sizeof(tb_image));
    if (all) tb_imgpx = all;
    if (list) tb_imgs = list;
    if (!all || !list) { free(px); tb_err = "не хватило памяти"; return -1; }
    memcpy(tb_imgpx + tb_imgpx_n, px, (size_t)w * h * 4);
    free(px);
    tb_imgs[tb_nimg].w = w; tb_imgs[tb_nimg].h = h; tb_imgs[tb_nimg].off = (uint32_t)tb_imgpx_n;
    tb_imgpx_n = need;
    tb_img_dirty = 1;
    return tb_nimg++;
}
// снимок кадра (что нарисовано сейчас) — новая картинка; потом draw_image рисует её за один раз
TB_API long long tb_screen_image(void) {
    if (!tb_px) { tb_err = "окно ещё не открыто"; return -1; }
    size_t n = (size_t)tb_gw * tb_gh, need = tb_imgpx_n + n;
    uint32_t *all = realloc(tb_imgpx, need * 4);
    if (all) tb_imgpx = all;
    tb_image *list = realloc(tb_imgs, (size_t)(tb_nimg + 1) * sizeof(tb_image));
    if (list) tb_imgs = list;
    if (!all || !list) return -1;
    for (size_t i = 0; i < n; i++) tb_imgpx[tb_imgpx_n + i] = tb_px[i] | 0xFF000000u;
    tb_imgs[tb_nimg].w = tb_gw; tb_imgs[tb_nimg].h = tb_gh; tb_imgs[tb_nimg].off = (uint32_t)tb_imgpx_n;
    tb_imgpx_n = need;
    tb_img_dirty = 1;
    return tb_nimg++;
}
TB_API long long tb_image_w(long long i) { return i >= 0 && i < tb_nimg ? tb_imgs[i].w : 0; }
TB_API long long tb_image_h(long long i) { return i >= 0 && i < tb_nimg ? tb_imgs[i].h : 0; }
// ---------- шрифты TTF: свой чтец (таблицы glyf, без чужих библиотек) ----------
// Буква рисуется на процессоре один раз в маленькую картинку с прозрачностью (сглаживание),
// дальше её рисуют и видеокарта, и процессор одинаково: точка = (цвет * a + фон * (255 - a)) / 255.
typedef struct { unsigned char *d; size_t n; uint32_t glyf, loca, hmtx, cmap; int upem, locfmt, nglyphs, nhm, asc, desc, cfmt; } tb_font_t;
TB_API tb_font_t *tb_fonts = 0;
TB_API int tb_nfonts = 0;
static unsigned tb_u16(const tb_font_t *f, size_t o) { return o + 2 <= f->n ? (unsigned)(f->d[o] << 8 | f->d[o + 1]) : 0; }
static int tb_s16(const tb_font_t *f, size_t o) { return (int16_t)tb_u16(f, o); }
static uint32_t tb_u32(const tb_font_t *f, size_t o) { return o + 4 <= f->n ? ((uint32_t)f->d[o] << 24 | (uint32_t)f->d[o + 1] << 16 | (uint32_t)f->d[o + 2] << 8 | f->d[o + 3]) : 0; }
static uint32_t tb_ttf_table(const tb_font_t *f, size_t base, const char *tag) {
    unsigned n = tb_u16(f, base + 4);
    for (unsigned i = 0; i < n; i++) {
        size_t e = base + 12 + 16 * (size_t)i;
        if (e + 16 <= f->n && memcmp(f->d + e, tag, 4) == 0) return tb_u32(f, e + 8);
    }
    return 0;
}
TB_API void tb_gpu_blend_probe(void);   // TB_GPU_CHECK: проба смешивания букв (ниже)
TB_API long long tb_load_font(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { tb_err = tb_cat("нет такого файла шрифта: ", path); return -1; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    unsigned char *d = sz > 12 ? malloc((size_t)sz) : 0;
    if (!d || fread(d, 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); free(d); tb_err = "не могу прочитать файл шрифта"; return -1; }
    fclose(fp);
    tb_font_t f = { d, (size_t)sz };
    size_t base = 0;
    if (memcmp(d, "ttcf", 4) == 0) base = tb_u32(&f, 12);   // сборник шрифтов (.ttc): берём первый
    uint32_t head = tb_ttf_table(&f, base, "head"), maxp = tb_ttf_table(&f, base, "maxp"), hhea = tb_ttf_table(&f, base, "hhea");
    f.glyf = tb_ttf_table(&f, base, "glyf"); f.loca = tb_ttf_table(&f, base, "loca");
    f.hmtx = tb_ttf_table(&f, base, "hmtx");
    uint32_t cmap = tb_ttf_table(&f, base, "cmap");
    if (!f.glyf || !f.loca) { free(d); tb_err = "этот шрифт не TrueType (контуры CFF/OTF пока не читаю): возьми .ttf"; return -1; }
    if (!head || !maxp || !hhea || !f.hmtx || !cmap) { free(d); tb_err = "шрифт сломан: нет нужных таблиц"; return -1; }
    f.upem = (int)tb_u16(&f, head + 18); f.locfmt = tb_s16(&f, head + 50);
    f.nglyphs = (int)tb_u16(&f, maxp + 4);
    f.asc = tb_s16(&f, hhea + 4); f.desc = tb_s16(&f, hhea + 6); f.nhm = (int)tb_u16(&f, hhea + 34);
    if (f.upem <= 0 || f.nhm <= 0 || f.asc - f.desc <= 0) { free(d); tb_err = "шрифт сломан: размеры"; return -1; }
    // буквы → номера глифов: Unicode-таблица формата 12 (все буквы) или 4 (до U+FFFF)
    unsigned nt = tb_u16(&f, cmap + 2);
    int best = 0;
    for (unsigned i = 0; i < nt; i++) {
        size_t e = cmap + 4 + 8 * (size_t)i;
        unsigned pid = tb_u16(&f, e), eid = tb_u16(&f, e + 2);
        uint32_t off = cmap + tb_u32(&f, e + 4);
        unsigned fmt = tb_u16(&f, off);
        int score = 0;
        if (fmt == 12 && (pid == 0 || (pid == 3 && eid == 10))) score = 3;
        else if (fmt == 4 && (pid == 0 || (pid == 3 && eid == 1))) score = 2;
        if (score > best) { best = score; f.cmap = off; f.cfmt = (int)fmt; }
    }
    if (!best) { free(d); tb_err = "в шрифте нет таблицы букв Unicode"; return -1; }
    tb_font_t *all = realloc(tb_fonts, (size_t)(tb_nfonts + 1) * sizeof(tb_font_t));
    if (!all) { free(d); tb_err = "не хватило памяти"; return -1; }
    tb_fonts = all;
    tb_fonts[tb_nfonts++] = f;
    tb_gpu_blend_probe();
    return tb_nfonts - 1;
}
static unsigned tb_ttf_glyph(const tb_font_t *f, unsigned u) {
    size_t c = f->cmap;
    if (f->cfmt == 12) {
        uint32_t n = tb_u32(f, c + 12);
        for (uint32_t lo = 0, hi = n; lo < hi;) {
            uint32_t mid = (lo + hi) / 2; size_t g = c + 16 + 12 * (size_t)mid;
            uint32_t a = tb_u32(f, g), b = tb_u32(f, g + 4);
            if (u < a) hi = mid; else if (u > b) lo = mid + 1; else return tb_u32(f, g + 8) + (u - a);
        }
        return 0;
    }
    if (u > 0xFFFF) return 0;
    unsigned segs = tb_u16(f, c + 6) / 2;
    size_t ends = c + 14, starts = ends + 2 * segs + 2, deltas = starts + 2 * segs, ranges = deltas + 2 * segs;
    for (unsigned i = 0; i < segs; i++) {
        if (u > tb_u16(f, ends + 2 * i)) continue;
        unsigned s = tb_u16(f, starts + 2 * i);
        if (u < s) return 0;
        unsigned delta = tb_u16(f, deltas + 2 * i), ro = tb_u16(f, ranges + 2 * i);
        if (ro == 0) return (u + delta) & 0xFFFF;
        unsigned g = tb_u16(f, ranges + 2 * i + ro + 2 * (u - s));
        return g ? (g + delta) & 0xFFFF : 0;
    }
    return 0;
}
static double tb_ttf_adv(const tb_font_t *f, unsigned g) {
    unsigned i = (int)g < f->nhm ? g : (unsigned)f->nhm - 1;
    return tb_u16(f, f->hmtx + 4 * (size_t)i);
}
static size_t tb_ttf_gpos(const tb_font_t *f, unsigned g, size_t *len) {
    if ((int)g >= f->nglyphs) { *len = 0; return 0; }
    size_t a, b;
    if (f->locfmt == 0) { a = 2 * (size_t)tb_u16(f, f->loca + 2 * g); b = 2 * (size_t)tb_u16(f, f->loca + 2 * g + 2); }
    else { a = tb_u32(f, f->loca + 4 * g); b = tb_u32(f, f->loca + 4 * g + 4); }
    *len = b > a ? b - a : 0;
    return f->glyf + a;
}
// контур буквы → отрезки (в точках экрана, y вниз); m — матрица шрифт → экран: x' = m0 x + m2 y + m4, y' = m1 x + m3 y + m5
typedef struct { float *e; int n, cap; } tb_edges;
static void tb_edge(tb_edges *E, double x0, double y0, double x1, double y1) {
    if (y0 == y1) return;
    if (E->n == E->cap) { E->cap = E->cap ? E->cap * 2 : 256; E->e = realloc(E->e, (size_t)E->cap * 4 * sizeof(float)); }
    float *p = E->e + 4 * E->n++;
    p[0] = (float)x0; p[1] = (float)y0; p[2] = (float)x1; p[3] = (float)y1;
}
static void tb_quad(tb_edges *E, double x0, double y0, double cx, double cy, double x1, double y1) {
    double dx = x0 - 2 * cx + x1, dy = y0 - 2 * cy + y1;
    int n = 1 + (int)sqrt(sqrt(dx * dx + dy * dy) * 4);
    if (n > 16) n = 16;
    double px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double qx = u * u * x0 + 2 * u * t * cx + t * t * x1, qy = u * u * y0 + 2 * u * t * cy + t * t * y1;
        tb_edge(E, px, py, qx, qy);
        px = qx; py = qy;
    }
}
static void tb_ttf_outline(const tb_font_t *f, unsigned g, const double *m, tb_edges *E, int depth) {
    size_t len, o = tb_ttf_gpos(f, g, &len);
    if (len < 10 || depth > 8) return;
    int nc = tb_s16(f, o);
    if (nc < 0) {   // составная буква (ё = е + точки): части со своим сдвигом и масштабом
        size_t p = o + 10;
        for (;;) {
            unsigned fl = tb_u16(f, p), sub = tb_u16(f, p + 2);
            p += 4;
            double dx = 0, dy = 0;
            if (fl & 1) { if (fl & 2) { dx = tb_s16(f, p); dy = tb_s16(f, p + 2); } p += 4; }
            else { if (fl & 2) { dx = (int8_t)f->d[p]; dy = (int8_t)f->d[p + 1]; } p += 2; }
            double a = 1, b = 0, c = 0, dd = 1;
            if (fl & 8) { a = dd = tb_s16(f, p) / 16384.0; p += 2; }
            else if (fl & 0x40) { a = tb_s16(f, p) / 16384.0; dd = tb_s16(f, p + 2) / 16384.0; p += 4; }
            else if (fl & 0x80) { a = tb_s16(f, p) / 16384.0; b = tb_s16(f, p + 2) / 16384.0; c = tb_s16(f, p + 4) / 16384.0; dd = tb_s16(f, p + 6) / 16384.0; p += 8; }
            double mm[6] = { m[0] * a + m[2] * b, m[1] * a + m[3] * b, m[0] * c + m[2] * dd, m[1] * c + m[3] * dd,
                             m[0] * dx + m[2] * dy + m[4], m[1] * dx + m[3] * dy + m[5] };
            tb_ttf_outline(f, sub, mm, E, depth + 1);
            if (!(fl & 0x20) || p >= f->n) break;
        }
        return;
    }
    size_t ends = o + 10;
    int npts = nc > 0 ? (int)tb_u16(f, ends + 2 * (size_t)(nc - 1)) + 1 : 0;
    if (npts <= 0 || npts > 20000) return;
    size_t p = ends + 2 * (size_t)nc;
    p += 2 + tb_u16(f, p);   // пропускаем команды подсказок (hinting)
    unsigned char *fl = malloc((size_t)npts);
    double *xy = malloc((size_t)npts * 2 * sizeof(double));
    for (int i = 0; i < npts && p < f->n;) {
        unsigned char v = f->d[p++];
        int rep = 1;
        if (v & 8) rep += p < f->n ? f->d[p++] : 0;
        while (rep-- > 0 && i < npts) fl[i++] = v;
    }
    int x = 0;
    for (int i = 0; i < npts; i++) {
        if (fl[i] & 2) { int v = p < f->n ? f->d[p++] : 0; x += (fl[i] & 16) ? v : -v; }
        else if (!(fl[i] & 16)) { x += tb_s16(f, p); p += 2; }
        xy[2 * i] = x;
    }
    int y = 0;
    for (int i = 0; i < npts; i++) {
        if (fl[i] & 4) { int v = p < f->n ? f->d[p++] : 0; y += (fl[i] & 32) ? v : -v; }
        else if (!(fl[i] & 32)) { y += tb_s16(f, p); p += 2; }
        xy[2 * i + 1] = y;
    }
    for (int i = 0; i < npts; i++) {
        double X = xy[2 * i], Y = xy[2 * i + 1];
        xy[2 * i] = m[0] * X + m[2] * Y + m[4];
        xy[2 * i + 1] = m[1] * X + m[3] * Y + m[5];
    }
    int start = 0;
    for (int c = 0; c < nc; c++) {
        int end = (int)tb_u16(f, ends + 2 * (size_t)c);
        if (end >= npts || end < start) break;
        int n = end - start + 1;
        // начинаем с точки на контуре (если такой нет — с середины между первыми двумя)
        int first = -1;
        for (int k = 0; k < n; k++) if (fl[start + k] & 1) { first = k; break; }
        double sx, sy;
        if (first >= 0) { sx = xy[2 * (start + first)]; sy = xy[2 * (start + first) + 1]; }
        else { first = 0; sx = (xy[2 * start] + xy[2 * (start + 1 < end ? start + 1 : start)]) / 2; sy = (xy[2 * start + 1] + xy[2 * (start + 1 < end ? start + 1 : start) + 1]) / 2; }
        double px = sx, py = sy, cx = 0, cy = 0;
        int have_c = 0;
        for (int k = 1; k <= n; k++) {
            int i = start + (first + k) % n;
            double X = xy[2 * i], Y = xy[2 * i + 1];
            if (fl[i] & 1) {
                if (have_c) tb_quad(E, px, py, cx, cy, X, Y); else tb_edge(E, px, py, X, Y);
                px = X; py = Y; have_c = 0;
            } else {
                if (have_c) {   // две точки-«магнита» подряд: между ними — точка на контуре
                    double mx = (cx + X) / 2, my = (cy + Y) / 2;
                    tb_quad(E, px, py, cx, cy, mx, my);
                    px = mx; py = my;
                }
                cx = X; cy = Y; have_c = 1;
            }
        }
        if (have_c) tb_quad(E, px, py, cx, cy, sx, sy); else tb_edge(E, px, py, sx, sy);
        start = end + 1;
    }
    free(fl); free(xy);
}
// закраска с сглаживанием: каждая сторона добавляет в клетки, сколько площади под ней (как font-rs)
static void tb_raster(const tb_edges *E, int w, int h, float *acc) {
    for (int k = 0; k < E->n; k++) {
        const float *e = E->e + 4 * k;
        double x0 = e[0], y0 = e[1], x1 = e[2], y1 = e[3], dir = 1;
        if (y0 > y1) { double t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dir = -1; }
        double dxdy = (x1 - x0) / (y1 - y0), x = x0;
        if (y0 < 0) { x -= y0 * dxdy; y0 = 0; }
        if (y1 > h) y1 = h;
        for (int yi = (int)floor(y0); yi < h && yi < y1; yi++) {
            double ya = yi > y0 ? yi : y0, yb = yi + 1 < y1 ? yi + 1 : y1, dy = yb - ya;
            if (dy <= 0) continue;
            double xn = x + dxdy * dy, d = dy * dir;
            double xa = x < xn ? x : xn, xb = x < xn ? xn : x;
            if (xa < 0) xa = 0;
            if (xb < 0) xb = 0;
            float *row = acc + (size_t)yi * (w + 2);
            int xai = (int)floor(xa), xbc = (int)ceil(xb);
            if (xai > w) xai = w;
            if (xbc > w + 1) xbc = w + 1;
            if (xbc <= xai + 1) {
                double xm = 0.5 * (x + xn) - xai;
                if (xm < 0) xm = 0;
                if (xm > 1) xm = 1;
                row[xai] += (float)(d - d * xm);
                row[xai + 1] += (float)(d * xm);
            } else {
                double s = 1 / (xb - xa), xaf = xa - xai, a0 = 0.5 * s * (1 - xaf) * (1 - xaf);
                double xbf = xb - xbc + 1, am = 0.5 * s * xbf * xbf;
                row[xai] += (float)(d * a0);
                if (xbc == xai + 2) row[xai + 1] += (float)(d * (1 - a0 - am));
                else {
                    double a1 = s * (1.5 - xaf);
                    row[xai + 1] += (float)(d * (a1 - a0));
                    for (int xi = xai + 2; xi < xbc - 1; xi++) row[xi] += (float)(d * s);
                    double a2 = a1 + (xbc - xai - 3) * s;
                    row[xbc - 1] += (float)(d * (1 - a2 - am));
                }
                row[xbc] += (float)(d * am);
            }
            x = xn;
        }
    }
}
// готовые буквы: шрифт + глиф + размер + цвет → картинка с прозрачностью и где её ставить
typedef struct { int font, glyph, size; uint32_t color; int img, left, top, used; } tb_gcache;
TB_API tb_gcache *tb_gc = 0;
TB_API int tb_gc_cap = 0, tb_gc_n = 0;
static tb_gcache *tb_glyph_img(int fi, unsigned g, int size, uint32_t color) {
    if (tb_gc_n * 2 >= tb_gc_cap) {   // таблица заполнилась наполовину: вдвое больше
        int cap = tb_gc_cap ? tb_gc_cap * 2 : 1024;
        tb_gcache *nt = calloc((size_t)cap, sizeof(tb_gcache));
        for (int i = 0; i < tb_gc_cap; i++) if (tb_gc[i].used) {
            unsigned hh = ((unsigned)tb_gc[i].font * 31u + (unsigned)tb_gc[i].glyph * 2654435761u + (unsigned)tb_gc[i].size * 40503u + tb_gc[i].color * 97u) & (unsigned)(cap - 1);
            while (nt[hh].used) hh = (hh + 1) & (unsigned)(cap - 1);
            nt[hh] = tb_gc[i];
        }
        free(tb_gc); tb_gc = nt; tb_gc_cap = cap;
    }
    unsigned hh = ((unsigned)fi * 31u + g * 2654435761u + (unsigned)size * 40503u + color * 97u) & (unsigned)(tb_gc_cap - 1);
    while (tb_gc[hh].used) {
        tb_gcache *c = &tb_gc[hh];
        if (c->font == fi && c->glyph == (int)g && c->size == size && c->color == color) return c;
        hh = (hh + 1) & (unsigned)(tb_gc_cap - 1);
    }
    tb_gcache *c = &tb_gc[hh];
    c->used = 1; c->font = fi; c->glyph = (int)g; c->size = size; c->color = color; c->img = -1;
    tb_gc_n++;
    const tb_font_t *f = &tb_fonts[fi];
    size_t len, o = tb_ttf_gpos(f, g, &len);
    if (len < 10) return c;   // пробел: рисовать нечего
    double s = (double)size / f->upem;
    int left = (int)floor(tb_s16(f, o + 2) * s) - 1, right = (int)ceil(tb_s16(f, o + 6) * s) + 1;
    int top = (int)floor(-tb_s16(f, o + 8) * s) - 1, bottom = (int)ceil(-tb_s16(f, o + 4) * s) + 1;
    int w = right - left, h = bottom - top;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return c;
    double m[6] = { s, 0, 0, -s, -left, -top };
    tb_edges E = { 0 };
    tb_ttf_outline(f, g, m, &E, 0);
    float *acc = calloc((size_t)(w + 2) * (h + 1), sizeof(float));
    tb_raster(&E, w, h, acc);
    free(E.e);
    size_t need = tb_imgpx_n + (size_t)w * h;
    uint32_t *all = realloc(tb_imgpx, need * 4);
    tb_image *list = realloc(tb_imgs, (size_t)(tb_nimg + 1) * sizeof(tb_image));
    if (all) tb_imgpx = all;
    if (list) tb_imgs = list;
    if (!all || !list) { free(acc); return c; }
    uint32_t *px = tb_imgpx + tb_imgpx_n;
    for (int j = 0; j < h; j++) {
        float sum = 0;
        const float *row = acc + (size_t)j * (w + 2);
        for (int i = 0; i < w; i++) {
            sum += row[i];
            float a = sum < 0 ? -sum : sum;
            int ai = a >= 1 ? 255 : (int)(a * 255 + 0.5f);
            px[j * w + i] = ((uint32_t)ai << 24) | (color & 0xFFFFFF);
        }
    }
    free(acc);
    tb_imgs[tb_nimg].w = w; tb_imgs[tb_nimg].h = h; tb_imgs[tb_nimg].off = (uint32_t)tb_imgpx_n;
    tb_imgpx_n = need;
    tb_img_dirty = 1;
    c->img = tb_nimg++; c->left = left; c->top = top;
    return c;
}
// высота строки — size * 5 / 4; линия букв — так, чтобы верх и низ шрифта легли в эту строку
static int tb_font_base(const tb_font_t *f, long long size) {
    double lh = size * 5 / 4.0;
    return (int)floor(lh * f->asc / (f->asc - f->desc) + 0.5);
}
static unsigned tb_utf8_next(const unsigned char **pp) {
    const unsigned char *p = *pp;
    unsigned u = *p++;
    if (u >= 0x80) {
        int more = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : u >= 0xC0 ? 1 : 0;
        u &= 0x3F >> more;
        while (more-- > 0 && (*p & 0xC0) == 0x80) u = (u << 6) | (*p++ & 0x3F);
    }
    *pp = p;
    return u;
}
TB_API long long tb_text_width(long long fi, const char *s, long long size) {
    if (fi < 0 || fi >= tb_nfonts || size <= 0) return 0;
    const tb_font_t *f = &tb_fonts[fi];
    double scale = (double)size / f->upem, pen = 0, best = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned u = tb_utf8_next(&p);
        if (u == '\n') { if (pen > best) best = pen; pen = 0; continue; }
        pen += tb_ttf_adv(f, tb_ttf_glyph(f, u)) * scale;
    }
    if (pen > best) best = pen;
    return (long long)floor(best + 0.5);
}
TB_API void tb_draw_image_a(long long id, long long x, long long y);
TB_API void tb_draw_font(long long fi, long long x, long long y, const char *s, long long size, long long c) {
    if (!tb_px || fi < 0 || fi >= tb_nfonts || size <= 0 || size > 1000) return;
    const tb_font_t *f = &tb_fonts[fi];
    double scale = (double)size / f->upem, pen = 0;
    long long base = y + tb_font_base(f, size), lh = size * 5 / 4;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned u = tb_utf8_next(&p);
        if (u == '\n') { pen = 0; base += lh; continue; }
        unsigned g = tb_ttf_glyph(f, u);
        long long gx = x + (long long)floor(pen + 0.5);
        pen += tb_ttf_adv(f, g) * scale;
        if (u == ' ' || gx >= tb_gw || base - 2 * size > tb_gh) continue;
        tb_gcache *gc = tb_glyph_img((int)fi, g, (int)size, (uint32_t)c & 0xFFFFFF);
        if (gc->img >= 0) tb_draw_image_a(gc->img, gx + gc->left, base + gc->top);
    }
}
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
// 3D: глубина. У треугольника сетки своя «плоскость глубины» A, B, C (целые числа, см. tb_tri3_depth);
// она лежит в tb_gplane[набор * TB_GI_CAP + номер фигуры], а в m — 2 | (это место << 2)
typedef struct { uint32_t a, b, c, pad; } tb_gplane_t;
TB_API tb_gplane_t *tb_gplane = 0;
TB_API int tb_gzclr = 1;               // видеокарта: в начале кадра стереть глубину
TB_API int tb_zclr = 1;                // процессор: то же для tb_zb
TB_API uint32_t *tb_zb = 0;            // процессор: глубина каждой точки
TB_API long long tb_zb_n = 0;
// 3D на видеокарте: сетка один раз ложится в память видеокарты (tb_mesh_pack), а на каждый draw_mesh
// пишется только запись: 3 строки по 4 целых — поворот (камера × поворот сетки) × 2^30 и сдвиг × 4096.
// В пачке фигур это одна особая фигура m = 3: x0 — сетка, y0 — какие грани не видны, x1 — первая запись,
// y1 — сколько записей подряд. Углы, экран и глубину считает видеокарта (tb3.vert) целыми числами —
// той же формулой, что процессор (tb_m3_corner): кадры совпадают точка в точку
typedef struct { int32_t r[12]; } tb_grec;
enum { TB_GR_CAP = 65536 };
TB_API tb_grec *tb_grec_p = 0;         // записи всех наборов подряд (видны видеокарте)
TB_API int tb_grn = 0;                 // сколько записей в текущем наборе
TB_API int tb_gpu_mesh = 0;            // 1: видеокарта умеет считать сетки сама (конвейер собрался)
TB_API int tb_mesh_gdirty = 1;         // сетки поменялись: перед рисованием залить заново
TB_API long long tb_gbatch = 1;        // номер текущей пачки (растёт после каждой отдачи видеокарте)
typedef struct { float size[2]; int32_t tbase, ntri, rbase, f, gw, gh, cull; } tb_gpc;   // постоянные шейдера сеток
TB_API long long tb_mesh_pack(int32_t *dst);   // все сетки подряд для видеокарты (dst = 0: только размер)
TB_API void tb_mesh_gpc(const tb_ginst *g, int slot, tb_gpc *pc);   // постоянные для фигуры m = 3
// TB_GSLOTS наборов памяти по кругу: пока видеокарта рисует один кадр, программа уже пишет следующий
enum { TB_GSLOTS = 3 };
TB_API int tb_gslot = 0;
TB_API uint32_t *tb_gpu_back = 0;      // TB_GPU_CHECK: кадр, прочитанный обратно с видеокарты
TB_API long long tb_gpu_frames = 0, tb_gpu_bad = 0;
TB_API void tb_gpu_flush(int present);   // своя у каждой системы: Metal или Vulkan
TB_API void tb_gpu_vsync(void);          // включить или выключить ожидание экрана (tb_gpu_nosync)
TB_API int tb_gpu_nosync = 0;
TB_API int tb_gpu_resize(int ow, int oh);   // окно поменяло размер
// окно можно тянуть мышью, если программа спросила его размер (window_w, window_h)
TB_API int tb_resizable = 0, tb_new_w = 0, tb_new_h = 0;
TB_API int tb_sharp = 0;       // window_sharp(): окно в настоящих точках экрана
TB_API double tb_sc = 1;        // во сколько раз точек больше (Retina — 2)
TB_API void tb_gfx_resizable(void);   // свои у каждой системы
TB_API void tb_gfx_reimage(void);            // 1: не ждать обновления экрана (set_fps(0), TB_FPS_FREE=1)
TB_API int tb_gpu_want(void) {
    const char *e = getenv("TB_GPU");
    tb_gpu_check = getenv("TB_GPU_CHECK") != 0 && getenv("TB_GPU_CHECK")[0] == '1';
    if (getenv("TB_FPS_FREE")) tb_gpu_nosync = 1;
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
// TB_GPU_CHECK: как видеокарта смешивает букву с фоном — таблица из пробы (tb_gpu_blend_probe)
TB_API int tb_blend_cal = 0;
TB_API uint8_t *tb_blend_lut = 0;   // [цвет][фон][a] → точка, 2^24 байт
TB_API int tb_gpu_probing = 0;   // TB_GPU_CHECK: идёт проба смешивания (tb_gpu_blend_probe), кадры не сверять
TB_API void tb_gpu_compare(void) {
    if (!tb_gpu_back || tb_gpu_probing) return;
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
TB_API void tb_gpu_image(long long id, long long x, long long y) {
    tb_image *im = &tb_imgs[id];
    if (x >= tb_gw || y >= tb_gh || x + im->w <= 0 || y + im->h <= 0) return;
    long long x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    long long x1 = x + im->w > tb_gw ? tb_gw : x + im->w, y1 = y + im->h > tb_gh ? tb_gh : y + im->h;
    tb_gpu_add(x0, y0, x1, y1, x, y, im->off, (3u << 30) | (uint32_t)im->w);
}
// картинка с настоящей прозрачностью (буквы шрифта): видеокарта смешивает с тем, что уже нарисовано
TB_API void tb_gpu_image_a(long long id, long long x, long long y) {
    tb_image *im = &tb_imgs[id];
    if (x >= tb_gw || y >= tb_gh || x + im->w <= 0 || y + im->h <= 0) return;
    long long x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    long long x1 = x + im->w > tb_gw ? tb_gw : x + im->w, y1 = y + im->h > tb_gh ? tb_gh : y + im->h;
    tb_gpu_add(x0, y0, x1, y1, x, y, im->off, (3u << 30) | (1u << 29) | (uint32_t)im->w);
}
// треугольник: m = 1 (вид 0, но не прямоугольник); в q и o — три угла, уже по кругу «как надо» (tb_tri_prep).
// Квадрат вокруг строит шейдер, а какие точки внутри — решает то же правило, что на процессоре (tb_tri_in)
TB_API void tb_gpu_triangle(const long long *t, long long c) {
    tb_gpu_add(t[0], t[1], t[2], t[3], t[4], t[5], (uint32_t)c & 0xFFFFFF, 1u);
}
// треугольник сетки (3D): как треугольник, плюс плоскость глубины в tb_gplane
TB_API void tb_gpu_tri3(const long long *t, long long c, const uint32_t *pl) {
    if (tb_gin == TB_GI_CAP) tb_gpu_flush(0);
    uint32_t at = (uint32_t)(tb_gslot * TB_GI_CAP + tb_gin);
    tb_gplane_t *g = &tb_gplane[at];
    g->a = pl[0]; g->b = pl[1]; g->c = pl[2]; g->pad = 0;
    tb_gpu_add(t[0], t[1], t[2], t[3], t[4], t[5], (uint32_t)c & 0xFFFFFF, 2u | (at << 2));
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
#if !defined(__APPLE__) && !defined(TB_BARE)
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
typedef struct { VkImageSubresourceLayers srcSubresource; VkOffset3D srcOffset; VkImageSubresourceLayers dstSubresource;
    VkOffset3D dstOffset; VkExtent3D extent; } VkImageCopy;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; uint32_t dynamicStateCount; const uint32_t *pDynamicStates; } VkPipelineDynamicStateCreateInfo;
typedef struct { VkDeviceSize bufferOffset; uint32_t bufferRowLength, bufferImageHeight; VkImageSubresourceLayers imageSubresource;
    VkOffset3D imageOffset; VkExtent3D imageExtent; } VkBufferImageCopy;
typedef struct { uint32_t sType; const void *pNext; uint32_t waitSemaphoreCount; const VkSemaphore *pWaitSemaphores;
    const VkFlags *pWaitDstStageMask; uint32_t commandBufferCount; const VkCommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount; const VkSemaphore *pSignalSemaphores; } VkSubmitInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t waitSemaphoreCount; const VkSemaphore *pWaitSemaphores;
    uint32_t swapchainCount; const VkSwapchainKHR *pSwapchains; const uint32_t *pImageIndices; VkResult *pResults; } VkPresentInfoKHR;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; } VkFenceCreateInfo;
typedef struct { VkFlags linearTilingFeatures, optimalTilingFeatures, bufferFeatures; } VkFormatProperties;
typedef struct { uint32_t failOp, passOp, depthFailOp, compareOp, compareMask, writeMask, reference; } VkStencilOpState;
typedef struct { uint32_t sType; const void *pNext; VkFlags flags; VkBool32 depthTestEnable, depthWriteEnable; uint32_t depthCompareOp;
    VkBool32 depthBoundsTestEnable, stencilTestEnable; VkStencilOpState front, back; float minDepthBounds, maxDepthBounds; } VkPipelineDepthStencilStateCreateInfo;
typedef struct { VkFlags aspectMask; uint32_t colorAttachment; VkClearColorValue clearValue; } VkClearAttachment;   // глубина — float32[0]
typedef struct { VkRect2D rect; uint32_t baseArrayLayer, layerCount; } VkClearRect;
// нужные функции Vulkan: имя, ответ, параметры
#define TBVK_FUNCS(X) \
    X(EnumeratePhysicalDevices, VkResult, (VkInstance, uint32_t *, VkPhysicalDevice *)) \
    X(GetPhysicalDeviceProperties, void, (VkPhysicalDevice, void *)) \
    X(GetPhysicalDeviceFeatures, void, (VkPhysicalDevice, VkBool32 *)) \
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
    X(DestroyBuffer, void, (VkDevice, VkBuffer, const void *)) \
    X(DestroyImage, void, (VkDevice, VkImage, const void *)) \
    X(DestroyImageView, void, (VkDevice, VkImageView, const void *)) \
    X(DestroyFramebuffer, void, (VkDevice, VkFramebuffer, const void *)) \
    X(CmdCopyImage, void, (VkCommandBuffer, VkImage, uint32_t, VkImage, uint32_t, uint32_t, const VkImageCopy *)) \
    X(CmdSetViewport, void, (VkCommandBuffer, uint32_t, uint32_t, const VkViewport *)) \
    X(CmdSetScissor, void, (VkCommandBuffer, uint32_t, uint32_t, const VkRect2D *)) \
    X(CmdClearAttachments, void, (VkCommandBuffer, uint32_t, const VkClearAttachment *, uint32_t, const VkClearRect *)) \
    X(FreeMemory, void, (VkDevice, VkDeviceMemory, const void *)) \
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
TB_API const uint32_t tb_spv_vert[947] = {
    0x07230203, 0x00010000, 0x0008000b, 0x00000099, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x000e000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000a, 0x00000010, 0x00000016,
    0x0000002e, 0x0000007c, 0x0000008d, 0x00000090, 0x00000093, 0x00000097, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000008, 0x00000076,
    0x00060005, 0x0000000a, 0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00030005, 0x0000000e,
    0x00000062, 0x00030005, 0x00000010, 0x00000071, 0x00030005, 0x00000016, 0x00006d63, 0x00030005,
    0x0000002e, 0x0000006f, 0x00030005, 0x0000004a, 0x00000050, 0x00050006, 0x0000004a, 0x00000000,
    0x657a6973, 0x00000000, 0x00030005, 0x0000004c, 0x00006370, 0x00030005, 0x0000005b, 0x00000070,
    0x00060005, 0x0000007a, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000007a,
    0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000007a, 0x00000001, 0x505f6c67,
    0x746e696f, 0x657a6953, 0x00000000, 0x00070006, 0x0000007a, 0x00000002, 0x435f6c67, 0x4470696c,
    0x61747369, 0x0065636e, 0x00070006, 0x0000007a, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369,
    0x0065636e, 0x00030005, 0x0000007c, 0x00000000, 0x00030005, 0x0000008d, 0x00006f76, 0x00030005,
    0x00000090, 0x006d6376, 0x00030005, 0x00000093, 0x00007176, 0x00030005, 0x00000097, 0x00007076,
    0x00040047, 0x0000000a, 0x0000000b, 0x0000002a, 0x00040047, 0x00000010, 0x0000001e, 0x00000000,
    0x00040047, 0x00000016, 0x0000001e, 0x00000002, 0x00040047, 0x0000002e, 0x0000001e, 0x00000001,
    0x00030047, 0x0000004a, 0x00000002, 0x00050048, 0x0000004a, 0x00000000, 0x00000023, 0x00000000,
    0x00030047, 0x0000007a, 0x00000002, 0x00050048, 0x0000007a, 0x00000000, 0x0000000b, 0x00000000,
    0x00050048, 0x0000007a, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000007a, 0x00000002,
    0x0000000b, 0x00000003, 0x00050048, 0x0000007a, 0x00000003, 0x0000000b, 0x00000004, 0x00030047,
    0x0000008d, 0x0000000e, 0x00040047, 0x0000008d, 0x0000001e, 0x00000000, 0x00030047, 0x00000090,
    0x0000000e, 0x00040047, 0x00000090, 0x0000001e, 0x00000001, 0x00030047, 0x00000093, 0x0000000e,
    0x00040047, 0x00000093, 0x0000001e, 0x00000002, 0x00030047, 0x00000097, 0x0000000e, 0x00040047,
    0x00000097, 0x0000001e, 0x00000003, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00040015, 0x00000006, 0x00000020, 0x00000001, 0x00040020, 0x00000007, 0x00000007, 0x00000006,
    0x00040020, 0x00000009, 0x00000001, 0x00000006, 0x0004003b, 0x00000009, 0x0000000a, 0x00000001,
    0x00040017, 0x0000000c, 0x00000006, 0x00000004, 0x00040020, 0x0000000d, 0x00000007, 0x0000000c,
    0x00040020, 0x0000000f, 0x00000001, 0x0000000c, 0x0004003b, 0x0000000f, 0x00000010, 0x00000001,
    0x00020014, 0x00000012, 0x00040015, 0x00000013, 0x00000020, 0x00000000, 0x00040017, 0x00000014,
    0x00000013, 0x00000002, 0x00040020, 0x00000015, 0x00000001, 0x00000014, 0x0004003b, 0x00000015,
    0x00000016, 0x00000001, 0x0004002b, 0x00000013, 0x00000017, 0x00000001, 0x00040020, 0x00000018,
    0x00000001, 0x00000013, 0x0004002b, 0x00000013, 0x0000001b, 0x00000000, 0x0004002b, 0x00000006,
    0x00000021, 0x0000001e, 0x00040017, 0x00000027, 0x00000006, 0x00000002, 0x00040020, 0x0000002d,
    0x00000001, 0x00000027, 0x0004003b, 0x0000002d, 0x0000002e, 0x00000001, 0x0004002b, 0x00000006,
    0x0000003c, 0x00000001, 0x0004002b, 0x00000013, 0x0000003f, 0x00000002, 0x0004002b, 0x00000013,
    0x00000042, 0x00000003, 0x0004002b, 0x00000006, 0x00000046, 0x00000000, 0x0007002c, 0x0000000c,
    0x00000047, 0x00000046, 0x00000046, 0x00000046, 0x00000046, 0x00030016, 0x00000048, 0x00000020,
    0x00040017, 0x00000049, 0x00000048, 0x00000002, 0x0003001e, 0x0000004a, 0x00000049, 0x00040020,
    0x0000004b, 0x00000009, 0x0000004a, 0x0004003b, 0x0000004b, 0x0000004c, 0x00000009, 0x00040020,
    0x0000004d, 0x00000009, 0x00000049, 0x00040020, 0x0000005a, 0x00000007, 0x00000049, 0x0004002b,
    0x00000006, 0x0000006a, 0x00000002, 0x00040017, 0x00000078, 0x00000048, 0x00000004, 0x0004001c,
    0x00000079, 0x00000048, 0x00000017, 0x0006001e, 0x0000007a, 0x00000078, 0x00000048, 0x00000079,
    0x00000079, 0x00040020, 0x0000007b, 0x00000003, 0x0000007a, 0x0004003b, 0x0000007b, 0x0000007c,
    0x00000003, 0x0004002b, 0x00000048, 0x00000081, 0x40000000, 0x0004002b, 0x00000048, 0x00000083,
    0x3f800000, 0x0004002b, 0x00000048, 0x00000086, 0x00000000, 0x00040020, 0x0000008a, 0x00000003,
    0x00000078, 0x00040020, 0x0000008c, 0x00000003, 0x00000027, 0x0004003b, 0x0000008c, 0x0000008d,
    0x00000003, 0x00040020, 0x0000008f, 0x00000003, 0x00000014, 0x0004003b, 0x0000008f, 0x00000090,
    0x00000003, 0x00040020, 0x00000092, 0x00000003, 0x0000000c, 0x0004003b, 0x00000092, 0x00000093,
    0x00000003, 0x00040017, 0x00000095, 0x00000013, 0x00000004, 0x00040020, 0x00000096, 0x00000003,
    0x00000095, 0x0004003b, 0x00000096, 0x00000097, 0x00000003, 0x0007002c, 0x00000095, 0x00000098,
    0x0000001b, 0x0000001b, 0x0000001b, 0x0000001b, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000007, 0x00000008, 0x00000007, 0x0004003b,
    0x0000000d, 0x0000000e, 0x00000007, 0x0004003b, 0x0000005a, 0x0000005b, 0x00000007, 0x0004003b,
    0x00000007, 0x0000005f, 0x00000007, 0x0004003b, 0x00000007, 0x0000006d, 0x00000007, 0x0004003d,
    0x00000006, 0x0000000b, 0x0000000a, 0x0003003e, 0x00000008, 0x0000000b, 0x0004003d, 0x0000000c,
    0x00000011, 0x00000010, 0x0003003e, 0x0000000e, 0x00000011, 0x00050041, 0x00000018, 0x00000019,
    0x00000016, 0x00000017, 0x0004003d, 0x00000013, 0x0000001a, 0x00000019, 0x000500ab, 0x00000012,
    0x0000001c, 0x0000001a, 0x0000001b, 0x000300f7, 0x0000001e, 0x00000000, 0x000400fa, 0x0000001c,
    0x0000001d, 0x0000001e, 0x000200f8, 0x0000001d, 0x00050041, 0x00000018, 0x0000001f, 0x00000016,
    0x00000017, 0x0004003d, 0x00000013, 0x00000020, 0x0000001f, 0x000500c2, 0x00000013, 0x00000022,
    0x00000020, 0x00000021, 0x000500aa, 0x00000012, 0x00000023, 0x00000022, 0x0000001b, 0x000200f9,
    0x0000001e, 0x000200f8, 0x0000001e, 0x000700f5, 0x00000012, 0x00000024, 0x0000001c, 0x00000005,
    0x00000023, 0x0000001d, 0x000300f7, 0x00000026, 0x00000000, 0x000400fa, 0x00000024, 0x00000025,
    0x00000026, 0x000200f8, 0x00000025, 0x0004003d, 0x0000000c, 0x00000028, 0x00000010, 0x0007004f,
    0x00000027, 0x00000029, 0x00000028, 0x00000028, 0x00000000, 0x00000001, 0x0004003d, 0x0000000c,
    0x0000002a, 0x00000010, 0x0007004f, 0x00000027, 0x0000002b, 0x0000002a, 0x0000002a, 0x00000002,
    0x00000003, 0x0007000c, 0x00000027, 0x0000002c, 0x00000001, 0x00000027, 0x00000029, 0x0000002b,
    0x0004003d, 0x00000027, 0x0000002f, 0x0000002e, 0x0007000c, 0x00000027, 0x00000030, 0x00000001,
    0x00000027, 0x0000002c, 0x0000002f, 0x00050041, 0x00000007, 0x00000031, 0x0000000e, 0x0000001b,
    0x00050051, 0x00000006, 0x00000032, 0x00000030, 0x00000000, 0x0003003e, 0x00000031, 0x00000032,
    0x00050041, 0x00000007, 0x00000033, 0x0000000e, 0x00000017, 0x00050051, 0x00000006, 0x00000034,
    0x00000030, 0x00000001, 0x0003003e, 0x00000033, 0x00000034, 0x0004003d, 0x0000000c, 0x00000035,
    0x00000010, 0x0007004f, 0x00000027, 0x00000036, 0x00000035, 0x00000035, 0x00000000, 0x00000001,
    0x0004003d, 0x0000000c, 0x00000037, 0x00000010, 0x0007004f, 0x00000027, 0x00000038, 0x00000037,
    0x00000037, 0x00000002, 0x00000003, 0x0007000c, 0x00000027, 0x00000039, 0x00000001, 0x0000002a,
    0x00000036, 0x00000038, 0x0004003d, 0x00000027, 0x0000003a, 0x0000002e, 0x0007000c, 0x00000027,
    0x0000003b, 0x00000001, 0x0000002a, 0x00000039, 0x0000003a, 0x00050050, 0x00000027, 0x0000003d,
    0x0000003c, 0x0000003c, 0x00050080, 0x00000027, 0x0000003e, 0x0000003b, 0x0000003d, 0x00050041,
    0x00000007, 0x00000040, 0x0000000e, 0x0000003f, 0x00050051, 0x00000006, 0x00000041, 0x0000003e,
    0x00000000, 0x0003003e, 0x00000040, 0x00000041, 0x00050041, 0x00000007, 0x00000043, 0x0000000e,
    0x00000042, 0x00050051, 0x00000006, 0x00000044, 0x0000003e, 0x00000001, 0x0003003e, 0x00000043,
    0x00000044, 0x0004003d, 0x0000000c, 0x00000045, 0x0000000e, 0x00050041, 0x0000004d, 0x0000004e,
    0x0000004c, 0x00000046, 0x0004003d, 0x00000049, 0x0000004f, 0x0000004e, 0x0004006e, 0x00000027,
    0x00000050, 0x0000004f, 0x00050041, 0x0000004d, 0x00000051, 0x0000004c, 0x00000046, 0x0004003d,
    0x00000049, 0x00000052, 0x00000051, 0x0004006e, 0x00000027, 0x00000053, 0x00000052, 0x00050051,
    0x00000006, 0x00000054, 0x00000050, 0x00000000, 0x00050051, 0x00000006, 0x00000055, 0x00000050,
    0x00000001, 0x00050051, 0x00000006, 0x00000056, 0x00000053, 0x00000000, 0x00050051, 0x00000006,
    0x00000057, 0x00000053, 0x00000001, 0x00070050, 0x0000000c, 0x00000058, 0x00000054, 0x00000055,
    0x00000056, 0x00000057, 0x0008000c, 0x0000000c, 0x00000059, 0x00000001, 0x0000002d, 0x00000045,
    0x00000047, 0x00000058, 0x0003003e, 0x0000000e, 0x00000059, 0x000200f9, 0x00000026, 0x000200f8,
    0x00000026, 0x0004003d, 0x00000006, 0x0000005c, 0x00000008, 0x000500c7, 0x00000006, 0x0000005d,
    0x0000005c, 0x0000003c, 0x000500ab, 0x00000012, 0x0000005e, 0x0000005d, 0x00000046, 0x000300f7,
    0x00000061, 0x00000000, 0x000400fa, 0x0000005e, 0x00000060, 0x00000064, 0x000200f8, 0x00000060,
    0x00050041, 0x00000007, 0x00000062, 0x0000000e, 0x0000003f, 0x0004003d, 0x00000006, 0x00000063,
    0x00000062, 0x0003003e, 0x0000005f, 0x00000063, 0x000200f9, 0x00000061, 0x000200f8, 0x00000064,
    0x00050041, 0x00000007, 0x00000065, 0x0000000e, 0x0000001b, 0x0004003d, 0x00000006, 0x00000066,
    0x00000065, 0x0003003e, 0x0000005f, 0x00000066, 0x000200f9, 0x00000061, 0x000200f8, 0x00000061,
    0x0004003d, 0x00000006, 0x00000067, 0x0000005f, 0x0004006f, 0x00000048, 0x00000068, 0x00000067,
    0x0004003d, 0x00000006, 0x00000069, 0x00000008, 0x000500c7, 0x00000006, 0x0000006b, 0x00000069,
    0x0000006a, 0x000500ab, 0x00000012, 0x0000006c, 0x0000006b, 0x00000046, 0x000300f7, 0x0000006f,
    0x00000000, 0x000400fa, 0x0000006c, 0x0000006e, 0x00000072, 0x000200f8, 0x0000006e, 0x00050041,
    0x00000007, 0x00000070, 0x0000000e, 0x00000042, 0x0004003d, 0x00000006, 0x00000071, 0x00000070,
    0x0003003e, 0x0000006d, 0x00000071, 0x000200f9, 0x0000006f, 0x000200f8, 0x00000072, 0x00050041,
    0x00000007, 0x00000073, 0x0000000e, 0x00000017, 0x0004003d, 0x00000006, 0x00000074, 0x00000073,
    0x0003003e, 0x0000006d, 0x00000074, 0x000200f9, 0x0000006f, 0x000200f8, 0x0000006f, 0x0004003d,
    0x00000006, 0x00000075, 0x0000006d, 0x0004006f, 0x00000048, 0x00000076, 0x00000075, 0x00050050,
    0x00000049, 0x00000077, 0x00000068, 0x00000076, 0x0003003e, 0x0000005b, 0x00000077, 0x0004003d,
    0x00000049, 0x0000007d, 0x0000005b, 0x00050041, 0x0000004d, 0x0000007e, 0x0000004c, 0x00000046,
    0x0004003d, 0x00000049, 0x0000007f, 0x0000007e, 0x00050088, 0x00000049, 0x00000080, 0x0000007d,
    0x0000007f, 0x0005008e, 0x00000049, 0x00000082, 0x00000080, 0x00000081, 0x00050050, 0x00000049,
    0x00000084, 0x00000083, 0x00000083, 0x00050083, 0x00000049, 0x00000085, 0x00000082, 0x00000084,
    0x00050051, 0x00000048, 0x00000087, 0x00000085, 0x00000000, 0x00050051, 0x00000048, 0x00000088,
    0x00000085, 0x00000001, 0x00070050, 0x00000078, 0x00000089, 0x00000087, 0x00000088, 0x00000086,
    0x00000083, 0x00050041, 0x0000008a, 0x0000008b, 0x0000007c, 0x00000046, 0x0003003e, 0x0000008b,
    0x00000089, 0x0004003d, 0x00000027, 0x0000008e, 0x0000002e, 0x0003003e, 0x0000008d, 0x0000008e,
    0x0004003d, 0x00000014, 0x00000091, 0x00000016, 0x0003003e, 0x00000090, 0x00000091, 0x0004003d,
    0x0000000c, 0x00000094, 0x00000010, 0x0003003e, 0x00000093, 0x00000094, 0x0003003e, 0x00000097,
    0x00000098, 0x000100fd, 0x00010038,
};
TB_API const uint32_t tb_spv_frag[2295] = {
    0x07230203, 0x00010000, 0x0008000b, 0x00000199, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x000c000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000055, 0x0000005e, 0x00000072,
    0x00000081, 0x000000ac, 0x00000173, 0x0000018c, 0x00030010, 0x00000004, 0x00000007, 0x00030010,
    0x00000004, 0x0000000c, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00070005, 0x0000000e, 0x65676465, 0x32697628, 0x3269763b, 0x3269763b, 0x0000003b,
    0x00030005, 0x0000000b, 0x00000061, 0x00030005, 0x0000000c, 0x00000062, 0x00030005, 0x0000000d,
    0x00000070, 0x00030005, 0x00000011, 0x00007865, 0x00030005, 0x00000019, 0x00007965, 0x00030005,
    0x00000020, 0x00000065, 0x00030005, 0x00000051, 0x00000070, 0x00060005, 0x00000055, 0x465f6c67,
    0x43676172, 0x64726f6f, 0x00000000, 0x00030005, 0x0000005b, 0x0000006d, 0x00030005, 0x0000005e,
    0x006d6376, 0x00030005, 0x00000062, 0x0000006b, 0x00040005, 0x00000067, 0x74706564, 0x00000068,
    0x00030005, 0x00000072, 0x00007176, 0x00040005, 0x00000073, 0x61726170, 0x0000006d, 0x00040005,
    0x00000076, 0x61726170, 0x0000006d, 0x00040005, 0x00000079, 0x61726170, 0x0000006d, 0x00030005,
    0x00000081, 0x00006f76, 0x00040005, 0x00000082, 0x61726170, 0x0000006d, 0x00040005, 0x00000085,
    0x61726170, 0x0000006d, 0x00040005, 0x00000087, 0x61726170, 0x0000006d, 0x00040005, 0x0000008f,
    0x61726170, 0x0000006d, 0x00040005, 0x00000091, 0x61726170, 0x0000006d, 0x00040005, 0x00000094,
    0x61726170, 0x0000006d, 0x00030005, 0x000000a4, 0x00000050, 0x00030005, 0x000000ac, 0x00007076,
    0x00030005, 0x000000b0, 0x00006c50, 0x00040006, 0x000000b0, 0x00000000, 0x00006c70, 0x00040005,
    0x000000b2, 0x6e616c70, 0x00007365, 0x00030005, 0x000000b9, 0x00006b6b, 0x00030005, 0x000000dc,
    0x00000072, 0x00030005, 0x000000e1, 0x00000069, 0x00030005, 0x000000e8, 0x0000006a, 0x00030005,
    0x00000103, 0x00000073, 0x00030005, 0x00000107, 0x00007863, 0x00030005, 0x0000010f, 0x00007972,
    0x00030005, 0x00000117, 0x00000067, 0x00030005, 0x0000011d, 0x00000062, 0x00040005, 0x00000120,
    0x746e6f46, 0x00000000, 0x00040006, 0x00000120, 0x00000000, 0x00000066, 0x00040005, 0x00000122,
    0x746e6f66, 0x00000000, 0x00030005, 0x0000013b, 0x00000063, 0x00030005, 0x0000013e, 0x00000061,
    0x00030005, 0x00000143, 0x00000076, 0x00030005, 0x00000145, 0x00676d49, 0x00040006, 0x00000145,
    0x00000000, 0x00007870, 0x00030005, 0x00000147, 0x00676d69, 0x00060005, 0x00000173, 0x465f6c67,
    0x44676172, 0x68747065, 0x00000000, 0x00030005, 0x00000177, 0x00006b6b, 0x00030005, 0x0000018c,
    0x006c6f63, 0x00040047, 0x00000055, 0x0000000b, 0x0000000f, 0x00030047, 0x0000005e, 0x0000000e,
    0x00040047, 0x0000005e, 0x0000001e, 0x00000001, 0x00030047, 0x00000072, 0x0000000e, 0x00040047,
    0x00000072, 0x0000001e, 0x00000002, 0x00030047, 0x00000081, 0x0000000e, 0x00040047, 0x00000081,
    0x0000001e, 0x00000000, 0x00030047, 0x000000ac, 0x0000000e, 0x00040047, 0x000000ac, 0x0000001e,
    0x00000003, 0x00040047, 0x000000af, 0x00000006, 0x00000010, 0x00030047, 0x000000b0, 0x00000003,
    0x00040048, 0x000000b0, 0x00000000, 0x00000018, 0x00050048, 0x000000b0, 0x00000000, 0x00000023,
    0x00000000, 0x00030047, 0x000000b2, 0x00000018, 0x00040047, 0x000000b2, 0x00000021, 0x00000002,
    0x00040047, 0x000000b2, 0x00000022, 0x00000000, 0x00040047, 0x0000011f, 0x00000006, 0x00000010,
    0x00030047, 0x00000120, 0x00000002, 0x00050048, 0x00000120, 0x00000000, 0x00000023, 0x00000000,
    0x00040047, 0x00000122, 0x00000021, 0x00000000, 0x00040047, 0x00000122, 0x00000022, 0x00000000,
    0x00040047, 0x00000144, 0x00000006, 0x00000004, 0x00030047, 0x00000145, 0x00000003, 0x00040048,
    0x00000145, 0x00000000, 0x00000018, 0x00050048, 0x00000145, 0x00000000, 0x00000023, 0x00000000,
    0x00030047, 0x00000147, 0x00000018, 0x00040047, 0x00000147, 0x00000021, 0x00000001, 0x00040047,
    0x00000147, 0x00000022, 0x00000000, 0x00040047, 0x00000173, 0x0000000b, 0x00000016, 0x00040047,
    0x0000018c, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00040015, 0x00000006, 0x00000020, 0x00000001, 0x00040017, 0x00000007, 0x00000006, 0x00000002,
    0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00020014, 0x00000009, 0x00060021, 0x0000000a,
    0x00000009, 0x00000008, 0x00000008, 0x00000008, 0x00040020, 0x00000010, 0x00000007, 0x00000006,
    0x00040015, 0x00000012, 0x00000020, 0x00000000, 0x0004002b, 0x00000012, 0x00000013, 0x00000000,
    0x0004002b, 0x00000012, 0x0000001a, 0x00000001, 0x0004002b, 0x00000006, 0x00000022, 0x00000002,
    0x0004002b, 0x00000006, 0x00000026, 0x00000001, 0x0004002b, 0x00000006, 0x00000039, 0x00000000,
    0x00030016, 0x00000052, 0x00000020, 0x00040017, 0x00000053, 0x00000052, 0x00000004, 0x00040020,
    0x00000054, 0x00000001, 0x00000053, 0x0004003b, 0x00000054, 0x00000055, 0x00000001, 0x00040017,
    0x00000056, 0x00000052, 0x00000002, 0x00040020, 0x0000005a, 0x00000007, 0x00000012, 0x00040017,
    0x0000005c, 0x00000012, 0x00000002, 0x00040020, 0x0000005d, 0x00000001, 0x0000005c, 0x0004003b,
    0x0000005d, 0x0000005e, 0x00000001, 0x00040020, 0x0000005f, 0x00000001, 0x00000012, 0x0004002b,
    0x00000006, 0x00000064, 0x0000001e, 0x00040020, 0x00000066, 0x00000007, 0x00000052, 0x0004002b,
    0x00000052, 0x00000068, 0x00000000, 0x00040017, 0x00000070, 0x00000006, 0x00000004, 0x00040020,
    0x00000071, 0x00000001, 0x00000070, 0x0004003b, 0x00000071, 0x00000072, 0x00000001, 0x00040020,
    0x00000080, 0x00000001, 0x00000007, 0x0004003b, 0x00000080, 0x00000081, 0x00000001, 0x0004002b,
    0x00000012, 0x0000009d, 0x00000002, 0x00040017, 0x000000a2, 0x00000012, 0x00000004, 0x00040020,
    0x000000a3, 0x00000007, 0x000000a2, 0x0004002b, 0x00000012, 0x000000a6, 0x00000003, 0x00040020,
    0x000000ab, 0x00000001, 0x000000a2, 0x0004003b, 0x000000ab, 0x000000ac, 0x00000001, 0x0003001d,
    0x000000af, 0x000000a2, 0x0003001e, 0x000000b0, 0x000000af, 0x00040020, 0x000000b1, 0x00000002,
    0x000000b0, 0x0004003b, 0x000000b1, 0x000000b2, 0x00000002, 0x00040020, 0x000000b5, 0x00000002,
    0x000000a2, 0x0004002b, 0x00000006, 0x000000d0, 0x00000006, 0x0004002b, 0x00000006, 0x000000d2,
    0x00ffffff, 0x0004002b, 0x00000052, 0x000000d5, 0x4b800000, 0x0004002b, 0x00000012, 0x000000de,
    0x003fffff, 0x00040020, 0x000000e4, 0x00000001, 0x00000006, 0x0004002b, 0x00000006, 0x00000119,
    0x00000016, 0x0004002b, 0x00000012, 0x0000011b, 0x000000ff, 0x0004002b, 0x00000012, 0x0000011e,
    0x000000a1, 0x0004001c, 0x0000011f, 0x000000a2, 0x0000011e, 0x0003001e, 0x00000120, 0x0000011f,
    0x00040020, 0x00000121, 0x00000002, 0x00000120, 0x0004003b, 0x00000121, 0x00000122, 0x00000002,
    0x00040020, 0x00000126, 0x00000002, 0x00000012, 0x0004002b, 0x00000006, 0x0000012a, 0x00000003,
    0x0004002b, 0x00000012, 0x0000012d, 0x00000008, 0x0004002b, 0x00000012, 0x00000132, 0x00000080,
    0x0003001d, 0x00000144, 0x00000012, 0x0003001e, 0x00000145, 0x00000144, 0x00040020, 0x00000146,
    0x00000002, 0x00000145, 0x0004003b, 0x00000146, 0x00000147, 0x00000002, 0x0004002b, 0x00000006,
    0x0000015e, 0x00000018, 0x0004002b, 0x00000012, 0x00000161, 0x20000000, 0x00040020, 0x00000172,
    0x00000003, 0x00000052, 0x0004003b, 0x00000172, 0x00000173, 0x00000003, 0x00040017, 0x00000175,
    0x00000012, 0x00000003, 0x00040020, 0x00000176, 0x00000007, 0x00000175, 0x0004002b, 0x00000006,
    0x00000179, 0x00000010, 0x0004002b, 0x00000006, 0x0000017d, 0x00000008, 0x0004002b, 0x00000012,
    0x00000186, 0x0000007f, 0x00040020, 0x0000018b, 0x00000003, 0x00000053, 0x0004003b, 0x0000018b,
    0x0000018c, 0x00000003, 0x00040017, 0x0000018e, 0x00000052, 0x00000003, 0x0004002b, 0x00000052,
    0x00000196, 0x437f0000, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x0004003b, 0x00000008, 0x00000051, 0x00000007, 0x0004003b, 0x0000005a, 0x0000005b,
    0x00000007, 0x0004003b, 0x0000005a, 0x00000062, 0x00000007, 0x0004003b, 0x00000066, 0x00000067,
    0x00000007, 0x0004003b, 0x00000008, 0x00000073, 0x00000007, 0x0004003b, 0x00000008, 0x00000076,
    0x00000007, 0x0004003b, 0x00000008, 0x00000079, 0x00000007, 0x0004003b, 0x00000008, 0x00000082,
    0x00000007, 0x0004003b, 0x00000008, 0x00000085, 0x00000007, 0x0004003b, 0x00000008, 0x00000087,
    0x00000007, 0x0004003b, 0x00000008, 0x0000008f, 0x00000007, 0x0004003b, 0x00000008, 0x00000091,
    0x00000007, 0x0004003b, 0x00000008, 0x00000094, 0x00000007, 0x0004003b, 0x000000a3, 0x000000a4,
    0x00000007, 0x0004003b, 0x000000a3, 0x000000a8, 0x00000007, 0x0004003b, 0x0000005a, 0x000000b9,
    0x00000007, 0x0004003b, 0x00000010, 0x000000dc, 0x00000007, 0x0004003b, 0x00000010, 0x000000e1,
    0x00000007, 0x0004003b, 0x00000010, 0x000000e8, 0x00000007, 0x0004003b, 0x00000010, 0x00000103,
    0x00000007, 0x0004003b, 0x00000010, 0x00000107, 0x00000007, 0x0004003b, 0x00000010, 0x0000010f,
    0x00000007, 0x0004003b, 0x0000005a, 0x00000117, 0x00000007, 0x0004003b, 0x0000005a, 0x0000011d,
    0x00000007, 0x0004003b, 0x0000005a, 0x0000013b, 0x00000007, 0x0004003b, 0x0000005a, 0x0000013e,
    0x00000007, 0x0004003b, 0x0000005a, 0x00000143, 0x00000007, 0x0004003b, 0x00000176, 0x00000177,
    0x00000007, 0x0004003d, 0x00000053, 0x00000057, 0x00000055, 0x0007004f, 0x00000056, 0x00000058,
    0x00000057, 0x00000057, 0x00000000, 0x00000001, 0x0004006e, 0x00000007, 0x00000059, 0x00000058,
    0x0003003e, 0x00000051, 0x00000059, 0x00050041, 0x0000005f, 0x00000060, 0x0000005e, 0x0000001a,
    0x0004003d, 0x00000012, 0x00000061, 0x00000060, 0x0003003e, 0x0000005b, 0x00000061, 0x0004003d,
    0x00000012, 0x00000063, 0x0000005b, 0x000500c2, 0x00000012, 0x00000065, 0x00000063, 0x00000064,
    0x0003003e, 0x00000062, 0x00000065, 0x0003003e, 0x00000067, 0x00000068, 0x0004003d, 0x00000012,
    0x00000069, 0x0000005b, 0x000500ab, 0x00000009, 0x0000006a, 0x00000069, 0x00000013, 0x0004003d,
    0x00000012, 0x0000006b, 0x00000062, 0x000500aa, 0x00000009, 0x0000006c, 0x0000006b, 0x00000013,
    0x000500a7, 0x00000009, 0x0000006d, 0x0000006a, 0x0000006c, 0x000300f7, 0x0000006f, 0x00000000,
    0x000400fa, 0x0000006d, 0x0000006e, 0x000000d7, 0x000200f8, 0x0000006e, 0x0004003d, 0x00000070,
    0x00000074, 0x00000072, 0x0007004f, 0x00000007, 0x00000075, 0x00000074, 0x00000074, 0x00000000,
    0x00000001, 0x0003003e, 0x00000073, 0x00000075, 0x0004003d, 0x00000070, 0x00000077, 0x00000072,
    0x0007004f, 0x00000007, 0x00000078, 0x00000077, 0x00000077, 0x00000002, 0x00000003, 0x0003003e,
    0x00000076, 0x00000078, 0x0004003d, 0x00000007, 0x0000007a, 0x00000051, 0x0003003e, 0x00000079,
    0x0000007a, 0x00070039, 0x00000009, 0x0000007b, 0x0000000e, 0x00000073, 0x00000076, 0x00000079,
    0x000400a8, 0x00000009, 0x0000007c, 0x0000007b, 0x000400a8, 0x00000009, 0x0000007d, 0x0000007c,
    0x000300f7, 0x0000007f, 0x00000000, 0x000400fa, 0x0000007d, 0x0000007e, 0x0000007f, 0x000200f8,
    0x0000007e, 0x0004003d, 0x00000070, 0x00000083, 0x00000072, 0x0007004f, 0x00000007, 0x00000084,
    0x00000083, 0x00000083, 0x00000002, 0x00000003, 0x0003003e, 0x00000082, 0x00000084, 0x0004003d,
    0x00000007, 0x00000086, 0x00000081, 0x0003003e, 0x00000085, 0x00000086, 0x0004003d, 0x00000007,
    0x00000088, 0x00000051, 0x0003003e, 0x00000087, 0x00000088, 0x00070039, 0x00000009, 0x00000089,
    0x0000000e, 0x00000082, 0x00000085, 0x00000087, 0x000400a8, 0x00000009, 0x0000008a, 0x00000089,
    0x000200f9, 0x0000007f, 0x000200f8, 0x0000007f, 0x000700f5, 0x00000009, 0x0000008b, 0x0000007c,
    0x0000006e, 0x0000008a, 0x0000007e, 0x000400a8, 0x00000009, 0x0000008c, 0x0000008b, 0x000300f7,
    0x0000008e, 0x00000000, 0x000400fa, 0x0000008c, 0x0000008d, 0x0000008e, 0x000200f8, 0x0000008d,
    0x0004003d, 0x00000007, 0x00000090, 0x00000081, 0x0003003e, 0x0000008f, 0x00000090, 0x0004003d,
    0x00000070, 0x00000092, 0x00000072, 0x0007004f, 0x00000007, 0x00000093, 0x00000092, 0x00000092,
    0x00000000, 0x00000001, 0x0003003e, 0x00000091, 0x00000093, 0x0004003d, 0x00000007, 0x00000095,
    0x00000051, 0x0003003e, 0x00000094, 0x00000095, 0x00070039, 0x00000009, 0x00000096, 0x0000000e,
    0x0000008f, 0x00000091, 0x00000094, 0x000400a8, 0x00000009, 0x00000097, 0x00000096, 0x000200f9,
    0x0000008e, 0x000200f8, 0x0000008e, 0x000700f5, 0x00000009, 0x00000098, 0x0000008b, 0x0000007f,
    0x00000097, 0x0000008d, 0x000300f7, 0x0000009a, 0x00000000, 0x000400fa, 0x00000098, 0x00000099,
    0x0000009a, 0x000200f8, 0x00000099, 0x000100fc, 0x000200f8, 0x0000009a, 0x0004003d, 0x00000012,
    0x0000009c, 0x0000005b, 0x000500c7, 0x00000012, 0x0000009e, 0x0000009c, 0x0000009d, 0x000500ab,
    0x00000009, 0x0000009f, 0x0000009e, 0x00000013, 0x000300f7, 0x000000a1, 0x00000000, 0x000400fa,
    0x0000009f, 0x000000a0, 0x000000a1, 0x000200f8, 0x000000a0, 0x0004003d, 0x00000012, 0x000000a5,
    0x0000005b, 0x000500aa, 0x00000009, 0x000000a7, 0x000000a5, 0x000000a6, 0x000300f7, 0x000000aa,
    0x00000000, 0x000400fa, 0x000000a7, 0x000000a9, 0x000000ae, 0x000200f8, 0x000000a9, 0x0004003d,
    0x000000a2, 0x000000ad, 0x000000ac, 0x0003003e, 0x000000a8, 0x000000ad, 0x000200f9, 0x000000aa,
    0x000200f8, 0x000000ae, 0x0004003d, 0x00000012, 0x000000b3, 0x0000005b, 0x000500c2, 0x00000012,
    0x000000b4, 0x000000b3, 0x00000022, 0x00060041, 0x000000b5, 0x000000b6, 0x000000b2, 0x00000039,
    0x000000b4, 0x0004003d, 0x000000a2, 0x000000b7, 0x000000b6, 0x0003003e, 0x000000a8, 0x000000b7,
    0x000200f9, 0x000000aa, 0x000200f8, 0x000000aa, 0x0004003d, 0x000000a2, 0x000000b8, 0x000000a8,
    0x0003003e, 0x000000a4, 0x000000b8, 0x00050041, 0x0000005a, 0x000000ba, 0x000000a4, 0x0000009d,
    0x0004003d, 0x00000012, 0x000000bb, 0x000000ba, 0x00050041, 0x0000005a, 0x000000bc, 0x000000a4,
    0x00000013, 0x0004003d, 0x00000012, 0x000000bd, 0x000000bc, 0x00050041, 0x00000010, 0x000000be,
    0x00000051, 0x00000013, 0x0004003d, 0x00000006, 0x000000bf, 0x000000be, 0x00050084, 0x00000006,
    0x000000c0, 0x00000022, 0x000000bf, 0x00050080, 0x00000006, 0x000000c1, 0x000000c0, 0x00000026,
    0x0004007c, 0x00000012, 0x000000c2, 0x000000c1, 0x00050084, 0x00000012, 0x000000c3, 0x000000bd,
    0x000000c2, 0x00050080, 0x00000012, 0x000000c4, 0x000000bb, 0x000000c3, 0x00050041, 0x0000005a,
    0x000000c5, 0x000000a4, 0x0000001a, 0x0004003d, 0x00000012, 0x000000c6, 0x000000c5, 0x00050041,
    0x00000010, 0x000000c7, 0x00000051, 0x0000001a, 0x0004003d, 0x00000006, 0x000000c8, 0x000000c7,
    0x00050084, 0x00000006, 0x000000c9, 0x00000022, 0x000000c8, 0x00050080, 0x00000006, 0x000000ca,
    0x000000c9, 0x00000026, 0x0004007c, 0x00000012, 0x000000cb, 0x000000ca, 0x00050084, 0x00000012,
    0x000000cc, 0x000000c6, 0x000000cb, 0x00050080, 0x00000012, 0x000000cd, 0x000000c4, 0x000000cc,
    0x0003003e, 0x000000b9, 0x000000cd, 0x0004003d, 0x00000012, 0x000000ce, 0x000000b9, 0x0004007c,
    0x00000006, 0x000000cf, 0x000000ce, 0x000500c3, 0x00000006, 0x000000d1, 0x000000cf, 0x000000d0,
    0x0008000c, 0x00000006, 0x000000d3, 0x00000001, 0x0000002d, 0x000000d1, 0x00000039, 0x000000d2,
    0x0004006f, 0x00000052, 0x000000d4, 0x000000d3, 0x00050088, 0x00000052, 0x000000d6, 0x000000d4,
    0x000000d5, 0x0003003e, 0x00000067, 0x000000d6, 0x000200f9, 0x000000a1, 0x000200f8, 0x000000a1,
    0x000200f9, 0x0000006f, 0x000200f8, 0x000000d7, 0x0004003d, 0x00000012, 0x000000d8, 0x00000062,
    0x000500aa, 0x00000009, 0x000000d9, 0x000000d8, 0x0000001a, 0x000300f7, 0x000000db, 0x00000000,
    0x000400fa, 0x000000d9, 0x000000da, 0x000000fe, 0x000200f8, 0x000000da, 0x0004003d, 0x00000012,
    0x000000dd, 0x0000005b, 0x000500c7, 0x00000012, 0x000000df, 0x000000dd, 0x000000de, 0x0004007c,
    0x00000006, 0x000000e0, 0x000000df, 0x0003003e, 0x000000dc, 0x000000e0, 0x00050041, 0x00000010,
    0x000000e2, 0x00000051, 0x00000013, 0x0004003d, 0x00000006, 0x000000e3, 0x000000e2, 0x00050041,
    0x000000e4, 0x000000e5, 0x00000081, 0x00000013, 0x0004003d, 0x00000006, 0x000000e6, 0x000000e5,
    0x00050082, 0x00000006, 0x000000e7, 0x000000e3, 0x000000e6, 0x0003003e, 0x000000e1, 0x000000e7,
    0x00050041, 0x00000010, 0x000000e9, 0x00000051, 0x0000001a, 0x0004003d, 0x00000006, 0x000000ea,
    0x000000e9, 0x00050041, 0x000000e4, 0x000000eb, 0x00000081, 0x0000001a, 0x0004003d, 0x00000006,
    0x000000ec, 0x000000eb, 0x00050082, 0x00000006, 0x000000ed, 0x000000ea, 0x000000ec, 0x0003003e,
    0x000000e8, 0x000000ed, 0x0004003d, 0x00000006, 0x000000ee, 0x000000e1, 0x0004003d, 0x00000006,
    0x000000ef, 0x000000e1, 0x00050084, 0x00000006, 0x000000f0, 0x000000ee, 0x000000ef, 0x0004003d,
    0x00000006, 0x000000f1, 0x000000e8, 0x0004003d, 0x00000006, 0x000000f2, 0x000000e8, 0x00050084,
    0x00000006, 0x000000f3, 0x000000f1, 0x000000f2, 0x00050080, 0x00000006, 0x000000f4, 0x000000f0,
    0x000000f3, 0x0004003d, 0x00000006, 0x000000f5, 0x000000dc, 0x0004003d, 0x00000006, 0x000000f6,
    0x000000dc, 0x00050084, 0x00000006, 0x000000f7, 0x000000f5, 0x000000f6, 0x0004003d, 0x00000006,
    0x000000f8, 0x000000dc, 0x00050080, 0x00000006, 0x000000f9, 0x000000f7, 0x000000f8, 0x000500ad,
    0x00000009, 0x000000fa, 0x000000f4, 0x000000f9, 0x000300f7, 0x000000fc, 0x00000000, 0x000400fa,
    0x000000fa, 0x000000fb, 0x000000fc, 0x000200f8, 0x000000fb, 0x000100fc, 0x000200f8, 0x000000fc,
    0x000200f9, 0x000000db, 0x000200f8, 0x000000fe, 0x0004003d, 0x00000012, 0x000000ff, 0x00000062,
    0x000500aa, 0x00000009, 0x00000100, 0x000000ff, 0x0000009d, 0x000300f7, 0x00000102, 0x00000000,
    0x000400fa, 0x00000100, 0x00000101, 0x00000102, 0x000200f8, 0x00000101, 0x0004003d, 0x00000012,
    0x00000104, 0x0000005b, 0x000500c7, 0x00000012, 0x00000105, 0x00000104, 0x000000de, 0x0004007c,
    0x00000006, 0x00000106, 0x00000105, 0x0003003e, 0x00000103, 0x00000106, 0x00050041, 0x00000010,
    0x00000108, 0x00000051, 0x00000013, 0x0004003d, 0x00000006, 0x00000109, 0x00000108, 0x00050041,
    0x000000e4, 0x0000010a, 0x00000081, 0x00000013, 0x0004003d, 0x00000006, 0x0000010b, 0x0000010a,
    0x00050082, 0x00000006, 0x0000010c, 0x00000109, 0x0000010b, 0x0004003d, 0x00000006, 0x0000010d,
    0x00000103, 0x00050087, 0x00000006, 0x0000010e, 0x0000010c, 0x0000010d, 0x0003003e, 0x00000107,
    0x0000010e, 0x00050041, 0x00000010, 0x00000110, 0x00000051, 0x0000001a, 0x0004003d, 0x00000006,
    0x00000111, 0x00000110, 0x00050041, 0x000000e4, 0x00000112, 0x00000081, 0x0000001a, 0x0004003d,
    0x00000006, 0x00000113, 0x00000112, 0x00050082, 0x00000006, 0x00000114, 0x00000111, 0x00000113,
    0x0004003d, 0x00000006, 0x00000115, 0x00000103, 0x00050087, 0x00000006, 0x00000116, 0x00000114,
    0x00000115, 0x0003003e, 0x0000010f, 0x00000116, 0x0004003d, 0x00000012, 0x00000118, 0x0000005b,
    0x000500c2, 0x00000012, 0x0000011a, 0x00000118, 0x00000119, 0x000500c7, 0x00000012, 0x0000011c,
    0x0000011a, 0x0000011b, 0x0003003e, 0x00000117, 0x0000011c, 0x0004003d, 0x00000012, 0x00000123,
    0x00000117, 0x0004003d, 0x00000006, 0x00000124, 0x0000010f, 0x000500c3, 0x00000006, 0x00000125,
    0x00000124, 0x00000022, 0x00070041, 0x00000126, 0x00000127, 0x00000122, 0x00000039, 0x00000123,
    0x00000125, 0x0004003d, 0x00000012, 0x00000128, 0x00000127, 0x0004003d, 0x00000006, 0x00000129,
    0x0000010f, 0x000500c7, 0x00000006, 0x0000012b, 0x00000129, 0x0000012a, 0x0004007c, 0x00000012,
    0x0000012c, 0x0000012b, 0x00050084, 0x00000012, 0x0000012e, 0x0000012c, 0x0000012d, 0x000500c2,
    0x00000012, 0x0000012f, 0x00000128, 0x0000012e, 0x000500c7, 0x00000012, 0x00000130, 0x0000012f,
    0x0000011b, 0x0003003e, 0x0000011d, 0x00000130, 0x0004003d, 0x00000012, 0x00000131, 0x0000011d,
    0x0004003d, 0x00000006, 0x00000133, 0x00000107, 0x0004007c, 0x00000012, 0x00000134, 0x00000133,
    0x000500c2, 0x00000012, 0x00000135, 0x00000132, 0x00000134, 0x000500c7, 0x00000012, 0x00000136,
    0x00000131, 0x00000135, 0x000500aa, 0x00000009, 0x00000137, 0x00000136, 0x00000013, 0x000300f7,
    0x00000139, 0x00000000, 0x000400fa, 0x00000137, 0x00000138, 0x00000139, 0x000200f8, 0x00000138,
    0x000100fc, 0x000200f8, 0x00000139, 0x000200f9, 0x00000102, 0x000200f8, 0x00000102, 0x000200f9,
    0x000000db, 0x000200f8, 0x000000db, 0x000200f9, 0x0000006f, 0x000200f8, 0x0000006f, 0x00050041,
    0x0000005f, 0x0000013c, 0x0000005e, 0x00000013, 0x0004003d, 0x00000012, 0x0000013d, 0x0000013c,
    0x0003003e, 0x0000013b, 0x0000013d, 0x0003003e, 0x0000013e, 0x0000011b, 0x0004003d, 0x00000012,
    0x0000013f, 0x00000062, 0x000500aa, 0x00000009, 0x00000140, 0x0000013f, 0x000000a6, 0x000300f7,
    0x00000142, 0x00000000, 0x000400fa, 0x00000140, 0x00000141, 0x00000142, 0x000200f8, 0x00000141,
    0x00050041, 0x0000005f, 0x00000148, 0x0000005e, 0x00000013, 0x0004003d, 0x00000012, 0x00000149,
    0x00000148, 0x00050041, 0x00000010, 0x0000014a, 0x00000051, 0x0000001a, 0x0004003d, 0x00000006,
    0x0000014b, 0x0000014a, 0x00050041, 0x000000e4, 0x0000014c, 0x00000081, 0x0000001a, 0x0004003d,
    0x00000006, 0x0000014d, 0x0000014c, 0x00050082, 0x00000006, 0x0000014e, 0x0000014b, 0x0000014d,
    0x0004007c, 0x00000012, 0x0000014f, 0x0000014e, 0x0004003d, 0x00000012, 0x00000150, 0x0000005b,
    0x000500c7, 0x00000012, 0x00000151, 0x00000150, 0x000000de, 0x00050084, 0x00000012, 0x00000152,
    0x0000014f, 0x00000151, 0x00050080, 0x00000012, 0x00000153, 0x00000149, 0x00000152, 0x00050041,
    0x00000010, 0x00000154, 0x00000051, 0x00000013, 0x0004003d, 0x00000006, 0x00000155, 0x00000154,
    0x00050041, 0x000000e4, 0x00000156, 0x00000081, 0x00000013, 0x0004003d, 0x00000006, 0x00000157,
    0x00000156, 0x00050082, 0x00000006, 0x00000158, 0x00000155, 0x00000157, 0x0004007c, 0x00000012,
    0x00000159, 0x00000158, 0x00050080, 0x00000012, 0x0000015a, 0x00000153, 0x00000159, 0x00060041,
    0x00000126, 0x0000015b, 0x00000147, 0x00000039, 0x0000015a, 0x0004003d, 0x00000012, 0x0000015c,
    0x0000015b, 0x0003003e, 0x00000143, 0x0000015c, 0x0004003d, 0x00000012, 0x0000015d, 0x00000143,
    0x000500c2, 0x00000012, 0x0000015f, 0x0000015d, 0x0000015e, 0x0003003e, 0x0000013e, 0x0000015f,
    0x0004003d, 0x00000012, 0x00000160, 0x0000005b, 0x000500c7, 0x00000012, 0x00000162, 0x00000160,
    0x00000161, 0x000500ab, 0x00000009, 0x00000163, 0x00000162, 0x00000013, 0x000300f7, 0x00000165,
    0x00000000, 0x000400fa, 0x00000163, 0x00000164, 0x0000016b, 0x000200f8, 0x00000164, 0x0004003d,
    0x00000012, 0x00000166, 0x0000013e, 0x000500aa, 0x00000009, 0x00000167, 0x00000166, 0x00000013,
    0x000300f7, 0x00000169, 0x00000000, 0x000400fa, 0x00000167, 0x00000168, 0x00000169, 0x000200f8,
    0x00000168, 0x000100fc, 0x000200f8, 0x00000169, 0x000200f9, 0x00000165, 0x000200f8, 0x0000016b,
    0x0004003d, 0x00000012, 0x0000016c, 0x0000013e, 0x000500b0, 0x00000009, 0x0000016d, 0x0000016c,
    0x00000132, 0x000300f7, 0x0000016f, 0x00000000, 0x000400fa, 0x0000016d, 0x0000016e, 0x0000016f,
    0x000200f8, 0x0000016e, 0x000100fc, 0x000200f8, 0x0000016f, 0x0003003e, 0x0000013e, 0x0000011b,
    0x000200f9, 0x00000165, 0x000200f8, 0x00000165, 0x0004003d, 0x00000012, 0x00000171, 0x00000143,
    0x0003003e, 0x0000013b, 0x00000171, 0x000200f9, 0x00000142, 0x000200f8, 0x00000142, 0x0004003d,
    0x00000052, 0x00000174, 0x00000067, 0x0003003e, 0x00000173, 0x00000174, 0x0004003d, 0x00000012,
    0x00000178, 0x0000013b, 0x000500c2, 0x00000012, 0x0000017a, 0x00000178, 0x00000179, 0x000500c7,
    0x00000012, 0x0000017b, 0x0000017a, 0x0000011b, 0x0004003d, 0x00000012, 0x0000017c, 0x0000013b,
    0x000500c2, 0x00000012, 0x0000017e, 0x0000017c, 0x0000017d, 0x000500c7, 0x00000012, 0x0000017f,
    0x0000017e, 0x0000011b, 0x0004003d, 0x00000012, 0x00000180, 0x0000013b, 0x000500c7, 0x00000012,
    0x00000181, 0x00000180, 0x0000011b, 0x00060050, 0x00000175, 0x00000182, 0x0000017b, 0x0000017f,
    0x00000181, 0x0004003d, 0x00000012, 0x00000183, 0x0000013e, 0x00060050, 0x00000175, 0x00000184,
    0x00000183, 0x00000183, 0x00000183, 0x00050084, 0x00000175, 0x00000185, 0x00000182, 0x00000184,
    0x00060050, 0x00000175, 0x00000187, 0x00000186, 0x00000186, 0x00000186, 0x00050080, 0x00000175,
    0x00000188, 0x00000185, 0x00000187, 0x00060050, 0x00000175, 0x00000189, 0x0000011b, 0x0000011b,
    0x0000011b, 0x00050086, 0x00000175, 0x0000018a, 0x00000188, 0x00000189, 0x0003003e, 0x00000177,
    0x0000018a, 0x0004003d, 0x00000175, 0x0000018d, 0x00000177, 0x00040070, 0x0000018e, 0x0000018f,
    0x0000018d, 0x0004003d, 0x00000012, 0x00000190, 0x0000013e, 0x00040070, 0x00000052, 0x00000191,
    0x00000190, 0x00050051, 0x00000052, 0x00000192, 0x0000018f, 0x00000000, 0x00050051, 0x00000052,
    0x00000193, 0x0000018f, 0x00000001, 0x00050051, 0x00000052, 0x00000194, 0x0000018f, 0x00000002,
    0x00070050, 0x00000053, 0x00000195, 0x00000192, 0x00000193, 0x00000194, 0x00000191, 0x00070050,
    0x00000053, 0x00000197, 0x00000196, 0x00000196, 0x00000196, 0x00000196, 0x00050088, 0x00000053,
    0x00000198, 0x00000195, 0x00000197, 0x0003003e, 0x0000018c, 0x00000198, 0x000100fd, 0x00010038,
    0x00050036, 0x00000009, 0x0000000e, 0x00000000, 0x0000000a, 0x00030037, 0x00000008, 0x0000000b,
    0x00030037, 0x00000008, 0x0000000c, 0x00030037, 0x00000008, 0x0000000d, 0x000200f8, 0x0000000f,
    0x0004003b, 0x00000010, 0x00000011, 0x00000007, 0x0004003b, 0x00000010, 0x00000019, 0x00000007,
    0x0004003b, 0x00000010, 0x00000020, 0x00000007, 0x00050041, 0x00000010, 0x00000014, 0x0000000c,
    0x00000013, 0x0004003d, 0x00000006, 0x00000015, 0x00000014, 0x00050041, 0x00000010, 0x00000016,
    0x0000000b, 0x00000013, 0x0004003d, 0x00000006, 0x00000017, 0x00000016, 0x00050082, 0x00000006,
    0x00000018, 0x00000015, 0x00000017, 0x0003003e, 0x00000011, 0x00000018, 0x00050041, 0x00000010,
    0x0000001b, 0x0000000c, 0x0000001a, 0x0004003d, 0x00000006, 0x0000001c, 0x0000001b, 0x00050041,
    0x00000010, 0x0000001d, 0x0000000b, 0x0000001a, 0x0004003d, 0x00000006, 0x0000001e, 0x0000001d,
    0x00050082, 0x00000006, 0x0000001f, 0x0000001c, 0x0000001e, 0x0003003e, 0x00000019, 0x0000001f,
    0x0004003d, 0x00000006, 0x00000021, 0x00000011, 0x00050041, 0x00000010, 0x00000023, 0x0000000d,
    0x0000001a, 0x0004003d, 0x00000006, 0x00000024, 0x00000023, 0x00050084, 0x00000006, 0x00000025,
    0x00000022, 0x00000024, 0x00050080, 0x00000006, 0x00000027, 0x00000025, 0x00000026, 0x00050041,
    0x00000010, 0x00000028, 0x0000000b, 0x0000001a, 0x0004003d, 0x00000006, 0x00000029, 0x00000028,
    0x00050084, 0x00000006, 0x0000002a, 0x00000022, 0x00000029, 0x00050082, 0x00000006, 0x0000002b,
    0x00000027, 0x0000002a, 0x00050084, 0x00000006, 0x0000002c, 0x00000021, 0x0000002b, 0x0004003d,
    0x00000006, 0x0000002d, 0x00000019, 0x00050041, 0x00000010, 0x0000002e, 0x0000000d, 0x00000013,
    0x0004003d, 0x00000006, 0x0000002f, 0x0000002e, 0x00050084, 0x00000006, 0x00000030, 0x00000022,
    0x0000002f, 0x00050080, 0x00000006, 0x00000031, 0x00000030, 0x00000026, 0x00050041, 0x00000010,
    0x00000032, 0x0000000b, 0x00000013, 0x0004003d, 0x00000006, 0x00000033, 0x00000032, 0x00050084,
    0x00000006, 0x00000034, 0x00000022, 0x00000033, 0x00050082, 0x00000006, 0x00000035, 0x00000031,
    0x00000034, 0x00050084, 0x00000006, 0x00000036, 0x0000002d, 0x00000035, 0x00050082, 0x00000006,
    0x00000037, 0x0000002c, 0x00000036, 0x0003003e, 0x00000020, 0x00000037, 0x0004003d, 0x00000006,
    0x00000038, 0x00000020, 0x000500ad, 0x00000009, 0x0000003a, 0x00000038, 0x00000039, 0x000400a8,
    0x00000009, 0x0000003b, 0x0000003a, 0x000300f7, 0x0000003d, 0x00000000, 0x000400fa, 0x0000003b,
    0x0000003c, 0x0000003d, 0x000200f8, 0x0000003c, 0x0004003d, 0x00000006, 0x0000003e, 0x00000020,
    0x000500aa, 0x00000009, 0x0000003f, 0x0000003e, 0x00000039, 0x000300f7, 0x00000041, 0x00000000,
    0x000400fa, 0x0000003f, 0x00000040, 0x00000041, 0x000200f8, 0x00000040, 0x0004003d, 0x00000006,
    0x00000042, 0x00000019, 0x000500ad, 0x00000009, 0x00000043, 0x00000042, 0x00000039, 0x000400a8,
    0x00000009, 0x00000044, 0x00000043, 0x000300f7, 0x00000046, 0x00000000, 0x000400fa, 0x00000044,
    0x00000045, 0x00000046, 0x000200f8, 0x00000045, 0x0004003d, 0x00000006, 0x00000047, 0x00000019,
    0x000500aa, 0x00000009, 0x00000048, 0x00000047, 0x00000039, 0x0004003d, 0x00000006, 0x00000049,
    0x00000011, 0x000500b1, 0x00000009, 0x0000004a, 0x00000049, 0x00000039, 0x000500a7, 0x00000009,
    0x0000004b, 0x00000048, 0x0000004a, 0x000200f9, 0x00000046, 0x000200f8, 0x00000046, 0x000700f5,
    0x00000009, 0x0000004c, 0x00000043, 0x00000040, 0x0000004b, 0x00000045, 0x000200f9, 0x00000041,
    0x000200f8, 0x00000041, 0x000700f5, 0x00000009, 0x0000004d, 0x0000003f, 0x0000003c, 0x0000004c,
    0x00000046, 0x000200f9, 0x0000003d, 0x000200f8, 0x0000003d, 0x000700f5, 0x00000009, 0x0000004e,
    0x0000003a, 0x0000000f, 0x0000004d, 0x00000041, 0x000200fe, 0x0000004e, 0x00010038,
};
TB_API const uint32_t tb_spv_vert3[3106] = {
    0x07230203, 0x00010000, 0x0008000b, 0x00000217, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x0000000b, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x000c000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x000000b0,
    0x000001c4, 0x000001e7, 0x000001f8, 0x000001fc, 0x00000204, 0x00000208, 0x00030003, 0x00000002,
    0x000001c2, 0x00070004, 0x415f4c47, 0x675f4252, 0x735f7570, 0x65646168, 0x6e695f72, 0x00343674,
    0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060005, 0x0000000b, 0x76696466, 0x34366928,
    0x36693b31, 0x003b3134, 0x00030005, 0x00000009, 0x00000061, 0x00030005, 0x0000000a, 0x00000062,
    0x00060005, 0x00000015, 0x6e726f63, 0x76287265, 0x693b3469, 0x00003b31, 0x00030005, 0x00000013,
    0x00000070, 0x00030005, 0x00000014, 0x00006972, 0x00030005, 0x00000034, 0x0000006b, 0x00030005,
    0x0000003e, 0x0000006d, 0x00030005, 0x00000040, 0x00636552, 0x00040006, 0x00000040, 0x00000000,
    0x00000072, 0x00030005, 0x00000042, 0x00636572, 0x00030005, 0x0000004d, 0x00000063, 0x00030005,
    0x00000073, 0x00000066, 0x00030005, 0x00000076, 0x00000050, 0x00050006, 0x00000076, 0x00000000,
    0x657a6973, 0x00000000, 0x00050006, 0x00000076, 0x00000001, 0x73616274, 0x00000065, 0x00050006,
    0x00000076, 0x00000002, 0x6972746e, 0x00000000, 0x00050006, 0x00000076, 0x00000003, 0x73616272,
    0x00000065, 0x00040006, 0x00000076, 0x00000004, 0x00000066, 0x00040006, 0x00000076, 0x00000005,
    0x00007767, 0x00040006, 0x00000076, 0x00000006, 0x00006867, 0x00050006, 0x00000076, 0x00000007,
    0x6c6c7563, 0x00000000, 0x00030005, 0x00000078, 0x00006370, 0x00040005, 0x00000089, 0x61726170,
    0x0000006d, 0x00040005, 0x0000008a, 0x61726170, 0x0000006d, 0x00040005, 0x0000009b, 0x61726170,
    0x0000006d, 0x00040005, 0x0000009c, 0x61726170, 0x0000006d, 0x00040005, 0x000000a5, 0x61726170,
    0x0000006d, 0x00040005, 0x000000a6, 0x61726170, 0x0000006d, 0x00030005, 0x000000ae, 0x00646969,
    0x00070005, 0x000000b0, 0x495f6c67, 0x6174736e, 0x4965636e, 0x7865646e, 0x00000000, 0x00030005,
    0x000000b2, 0x00000064, 0x00030005, 0x000000b7, 0x00000074, 0x00030005, 0x000000be, 0x00007274,
    0x00040005, 0x000000c0, 0x6873654d, 0x00000000, 0x00040006, 0x000000c0, 0x00000000, 0x00000076,
    0x00040005, 0x000000c2, 0x6873656d, 0x00000000, 0x00030005, 0x000000c9, 0x00006972, 0x00030005,
    0x000000d0, 0x00000061, 0x00040005, 0x000000d3, 0x61726170, 0x0000006d, 0x00040005, 0x000000d6,
    0x61726170, 0x0000006d, 0x00030005, 0x000000d9, 0x00000062, 0x00040005, 0x000000dc, 0x61726170,
    0x0000006d, 0x00040005, 0x000000df, 0x61726170, 0x0000006d, 0x00030005, 0x000000e2, 0x00000063,
    0x00040005, 0x000000e5, 0x61726170, 0x0000006d, 0x00040005, 0x000000e8, 0x61726170, 0x0000006d,
    0x00040005, 0x000000eb, 0x61657261, 0x00000000, 0x00040005, 0x00000104, 0x70696b73, 0x00000000,
    0x00030005, 0x00000120, 0x0000006b, 0x00030005, 0x0000012a, 0x00783165, 0x00030005, 0x00000130,
    0x00793165, 0x00030005, 0x00000136, 0x00783265, 0x00030005, 0x0000013c, 0x00793265, 0x00030005,
    0x00000142, 0x00003164, 0x00030005, 0x00000148, 0x00003264, 0x00030005, 0x0000014e, 0x00000041,
    0x00040005, 0x0000015d, 0x61726170, 0x0000006d, 0x00040005, 0x0000015e, 0x61726170, 0x0000006d,
    0x00030005, 0x00000160, 0x00000042, 0x00040005, 0x0000016d, 0x61726170, 0x0000006d, 0x00040005,
    0x0000016e, 0x61726170, 0x0000006d, 0x00030005, 0x00000170, 0x00000043, 0x00030005, 0x00000180,
    0x00000071, 0x00030005, 0x00000190, 0x0000006f, 0x00030005, 0x00000198, 0x00006262, 0x00030005,
    0x000001c3, 0x00000076, 0x00060005, 0x000001c4, 0x565f6c67, 0x65747265, 0x646e4978, 0x00007865,
    0x00030005, 0x000001c7, 0x00000070, 0x00060005, 0x000001e5, 0x505f6c67, 0x65567265, 0x78657472,
    0x00000000, 0x00060006, 0x000001e5, 0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006,
    0x000001e5, 0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000, 0x00070006, 0x000001e5,
    0x00000002, 0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e, 0x00070006, 0x000001e5, 0x00000003,
    0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e, 0x00030005, 0x000001e7, 0x00000000, 0x00030005,
    0x000001f8, 0x00006f76, 0x00030005, 0x000001fc, 0x006d6376, 0x00030005, 0x00000204, 0x00007176,
    0x00030005, 0x00000208, 0x00007076, 0x00040047, 0x0000003f, 0x00000006, 0x00000010, 0x00030047,
    0x00000040, 0x00000003, 0x00040048, 0x00000040, 0x00000000, 0x00000018, 0x00050048, 0x00000040,
    0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x00000042, 0x00000018, 0x00040047, 0x00000042,
    0x00000021, 0x00000004, 0x00040047, 0x00000042, 0x00000022, 0x00000000, 0x00030047, 0x00000076,
    0x00000002, 0x00050048, 0x00000076, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000076,
    0x00000001, 0x00000023, 0x00000008, 0x00050048, 0x00000076, 0x00000002, 0x00000023, 0x0000000c,
    0x00050048, 0x00000076, 0x00000003, 0x00000023, 0x00000010, 0x00050048, 0x00000076, 0x00000004,
    0x00000023, 0x00000014, 0x00050048, 0x00000076, 0x00000005, 0x00000023, 0x00000018, 0x00050048,
    0x00000076, 0x00000006, 0x00000023, 0x0000001c, 0x00050048, 0x00000076, 0x00000007, 0x00000023,
    0x00000020, 0x00040047, 0x000000b0, 0x0000000b, 0x0000002b, 0x00040047, 0x000000bf, 0x00000006,
    0x00000010, 0x00030047, 0x000000c0, 0x00000003, 0x00040048, 0x000000c0, 0x00000000, 0x00000018,
    0x00050048, 0x000000c0, 0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x000000c2, 0x00000018,
    0x00040047, 0x000000c2, 0x00000021, 0x00000003, 0x00040047, 0x000000c2, 0x00000022, 0x00000000,
    0x00040047, 0x000001c4, 0x0000000b, 0x0000002a, 0x00030047, 0x000001e5, 0x00000002, 0x00050048,
    0x000001e5, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x000001e5, 0x00000001, 0x0000000b,
    0x00000001, 0x00050048, 0x000001e5, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x000001e5,
    0x00000003, 0x0000000b, 0x00000004, 0x00030047, 0x000001f8, 0x0000000e, 0x00040047, 0x000001f8,
    0x0000001e, 0x00000000, 0x00030047, 0x000001fc, 0x0000000e, 0x00040047, 0x000001fc, 0x0000001e,
    0x00000001, 0x00030047, 0x00000204, 0x0000000e, 0x00040047, 0x00000204, 0x0000001e, 0x00000002,
    0x00030047, 0x00000208, 0x0000000e, 0x00040047, 0x00000208, 0x0000001e, 0x00000003, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000040, 0x00000001,
    0x00040020, 0x00000007, 0x00000007, 0x00000006, 0x00050021, 0x00000008, 0x00000006, 0x00000007,
    0x00000007, 0x00040015, 0x0000000d, 0x00000020, 0x00000001, 0x00040017, 0x0000000e, 0x0000000d,
    0x00000004, 0x00040020, 0x0000000f, 0x00000007, 0x0000000e, 0x00040020, 0x00000010, 0x00000007,
    0x0000000d, 0x00040017, 0x00000011, 0x00000006, 0x00000003, 0x00050021, 0x00000012, 0x00000011,
    0x0000000f, 0x00000010, 0x0005002b, 0x00000006, 0x00000018, 0x00000000, 0x00000000, 0x00020014,
    0x00000019, 0x00040015, 0x0000001e, 0x00000040, 0x00000000, 0x0005002b, 0x0000001e, 0x0000002b,
    0x00000001, 0x00000000, 0x0004002b, 0x0000000d, 0x00000035, 0x00000000, 0x0004002b, 0x0000000d,
    0x0000003c, 0x00000003, 0x0003001d, 0x0000003f, 0x0000000e, 0x0003001e, 0x00000040, 0x0000003f,
    0x00040020, 0x00000041, 0x00000002, 0x00000040, 0x0004003b, 0x00000041, 0x00000042, 0x00000002,
    0x00040020, 0x00000046, 0x00000002, 0x0000000e, 0x00040015, 0x00000049, 0x00000020, 0x00000000,
    0x0004002b, 0x00000049, 0x0000004a, 0x00000003, 0x0004001c, 0x0000004b, 0x00000006, 0x0000004a,
    0x00040020, 0x0000004c, 0x00000007, 0x0000004b, 0x0004002b, 0x00000049, 0x0000004f, 0x00000000,
    0x0004002b, 0x00000049, 0x00000057, 0x00000001, 0x0004002b, 0x00000049, 0x00000060, 0x00000002,
    0x0004002b, 0x0000000d, 0x00000069, 0x0000001e, 0x0004002b, 0x0000000d, 0x00000071, 0x00000001,
    0x00030016, 0x00000074, 0x00000020, 0x00040017, 0x00000075, 0x00000074, 0x00000002, 0x000a001e,
    0x00000076, 0x00000075, 0x0000000d, 0x0000000d, 0x0000000d, 0x0000000d, 0x0000000d, 0x0000000d,
    0x0000000d, 0x00040020, 0x00000077, 0x00000009, 0x00000076, 0x0004003b, 0x00000077, 0x00000078,
    0x00000009, 0x0004002b, 0x0000000d, 0x00000079, 0x00000004, 0x00040020, 0x0000007a, 0x00000009,
    0x0000000d, 0x0004002b, 0x0000000d, 0x0000007e, 0x00000005, 0x0005002b, 0x00000006, 0x00000082,
    0x00000080, 0x00000000, 0x0004002b, 0x0000000d, 0x00000088, 0x00000002, 0x0004002b, 0x0000000d,
    0x00000090, 0x00000008, 0x0004002b, 0x0000000d, 0x00000092, 0x00000006, 0x0005002b, 0x00000006,
    0x000000a3, 0x01000000, 0x00000000, 0x0005002b, 0x00000006, 0x000000a4, 0xcd000000, 0x00000000,
    0x00040020, 0x000000af, 0x00000001, 0x0000000d, 0x0004003b, 0x000000af, 0x000000b0, 0x00000001,
    0x0003001d, 0x000000bf, 0x0000000e, 0x0003001e, 0x000000c0, 0x000000bf, 0x00040020, 0x000000c1,
    0x00000002, 0x000000c0, 0x0004003b, 0x000000c1, 0x000000c2, 0x00000002, 0x00040020, 0x000000cf,
    0x00000007, 0x00000011, 0x00040020, 0x00000103, 0x00000007, 0x00000019, 0x0004002b, 0x0000000d,
    0x0000010a, 0x00000007, 0x0005002b, 0x00000006, 0x00000129, 0x00000001, 0x00000000, 0x0005002b,
    0x00000006, 0x0000014f, 0x00000040, 0x00000000, 0x0005002b, 0x00000006, 0x0000015a, 0x00000002,
    0x00000000, 0x00040017, 0x0000018e, 0x0000000d, 0x00000002, 0x00040020, 0x0000018f, 0x00000007,
    0x0000018e, 0x0007002c, 0x0000000e, 0x000001b2, 0x00000035, 0x00000035, 0x00000035, 0x00000035,
    0x00040020, 0x000001b3, 0x00000009, 0x00000075, 0x0004003b, 0x000000af, 0x000001c4, 0x00000001,
    0x00040020, 0x000001c6, 0x00000007, 0x00000075, 0x00040017, 0x000001e3, 0x00000074, 0x00000004,
    0x0004001c, 0x000001e4, 0x00000074, 0x00000057, 0x0006001e, 0x000001e5, 0x000001e3, 0x00000074,
    0x000001e4, 0x000001e4, 0x00040020, 0x000001e6, 0x00000003, 0x000001e5, 0x0004003b, 0x000001e6,
    0x000001e7, 0x00000003, 0x0004002b, 0x00000074, 0x000001ec, 0x40000000, 0x0004002b, 0x00000074,
    0x000001ee, 0x3f800000, 0x0004002b, 0x00000074, 0x000001f1, 0x00000000, 0x00040020, 0x000001f5,
    0x00000003, 0x000001e3, 0x00040020, 0x000001f7, 0x00000003, 0x0000018e, 0x0004003b, 0x000001f7,
    0x000001f8, 0x00000003, 0x00040017, 0x000001fa, 0x00000049, 0x00000002, 0x00040020, 0x000001fb,
    0x00000003, 0x000001fa, 0x0004003b, 0x000001fb, 0x000001fc, 0x00000003, 0x0004002b, 0x00000049,
    0x00000200, 0x00ffffff, 0x00040020, 0x00000203, 0x00000003, 0x0000000e, 0x0004003b, 0x00000203,
    0x00000204, 0x00000003, 0x00040017, 0x00000206, 0x00000049, 0x00000004, 0x00040020, 0x00000207,
    0x00000003, 0x00000206, 0x0004003b, 0x00000207, 0x00000208, 0x00000003, 0x0005002b, 0x0000001e,
    0x0000020b, 0xffffffff, 0x00000000, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x0004003b, 0x00000010, 0x000000ae, 0x00000007, 0x0004003b, 0x00000010,
    0x000000b2, 0x00000007, 0x0004003b, 0x00000010, 0x000000b7, 0x00000007, 0x0004003b, 0x0000000f,
    0x000000be, 0x00000007, 0x0004003b, 0x00000010, 0x000000c9, 0x00000007, 0x0004003b, 0x000000cf,
    0x000000d0, 0x00000007, 0x0004003b, 0x0000000f, 0x000000d3, 0x00000007, 0x0004003b, 0x00000010,
    0x000000d6, 0x00000007, 0x0004003b, 0x000000cf, 0x000000d9, 0x00000007, 0x0004003b, 0x0000000f,
    0x000000dc, 0x00000007, 0x0004003b, 0x00000010, 0x000000df, 0x00000007, 0x0004003b, 0x000000cf,
    0x000000e2, 0x00000007, 0x0004003b, 0x0000000f, 0x000000e5, 0x00000007, 0x0004003b, 0x00000010,
    0x000000e8, 0x00000007, 0x0004003b, 0x00000007, 0x000000eb, 0x00000007, 0x0004003b, 0x00000103,
    0x00000104, 0x00000007, 0x0004003b, 0x000000cf, 0x00000120, 0x00000007, 0x0004003b, 0x00000007,
    0x0000012a, 0x00000007, 0x0004003b, 0x00000007, 0x00000130, 0x00000007, 0x0004003b, 0x00000007,
    0x00000136, 0x00000007, 0x0004003b, 0x00000007, 0x0000013c, 0x00000007, 0x0004003b, 0x00000007,
    0x00000142, 0x00000007, 0x0004003b, 0x00000007, 0x00000148, 0x00000007, 0x0004003b, 0x00000007,
    0x0000014e, 0x00000007, 0x0004003b, 0x00000007, 0x0000015d, 0x00000007, 0x0004003b, 0x00000007,
    0x0000015e, 0x00000007, 0x0004003b, 0x00000007, 0x00000160, 0x00000007, 0x0004003b, 0x00000007,
    0x0000016d, 0x00000007, 0x0004003b, 0x00000007, 0x0000016e, 0x00000007, 0x0004003b, 0x00000007,
    0x00000170, 0x00000007, 0x0004003b, 0x0000000f, 0x00000180, 0x00000007, 0x0004003b, 0x0000018f,
    0x00000190, 0x00000007, 0x0004003b, 0x0000000f, 0x00000198, 0x00000007, 0x0004003b, 0x00000010,
    0x000001c3, 0x00000007, 0x0004003b, 0x000001c6, 0x000001c7, 0x00000007, 0x0004003b, 0x00000010,
    0x000001cb, 0x00000007, 0x0004003b, 0x00000010, 0x000001d8, 0x00000007, 0x0004003d, 0x0000000d,
    0x000000b1, 0x000000b0, 0x0003003e, 0x000000ae, 0x000000b1, 0x0004003d, 0x0000000d, 0x000000b3,
    0x000000ae, 0x00050041, 0x0000007a, 0x000000b4, 0x00000078, 0x00000088, 0x0004003d, 0x0000000d,
    0x000000b5, 0x000000b4, 0x00050087, 0x0000000d, 0x000000b6, 0x000000b3, 0x000000b5, 0x0003003e,
    0x000000b2, 0x000000b6, 0x0004003d, 0x0000000d, 0x000000b8, 0x000000ae, 0x0004003d, 0x0000000d,
    0x000000b9, 0x000000b2, 0x00050041, 0x0000007a, 0x000000ba, 0x00000078, 0x00000088, 0x0004003d,
    0x0000000d, 0x000000bb, 0x000000ba, 0x00050084, 0x0000000d, 0x000000bc, 0x000000b9, 0x000000bb,
    0x00050082, 0x0000000d, 0x000000bd, 0x000000b8, 0x000000bc, 0x0003003e, 0x000000b7, 0x000000bd,
    0x00050041, 0x0000007a, 0x000000c3, 0x00000078, 0x00000071, 0x0004003d, 0x0000000d, 0x000000c4,
    0x000000c3, 0x0004003d, 0x0000000d, 0x000000c5, 0x000000b7, 0x00050080, 0x0000000d, 0x000000c6,
    0x000000c4, 0x000000c5, 0x00060041, 0x00000046, 0x000000c7, 0x000000c2, 0x00000035, 0x000000c6,
    0x0004003d, 0x0000000e, 0x000000c8, 0x000000c7, 0x0003003e, 0x000000be, 0x000000c8, 0x00050041,
    0x0000007a, 0x000000ca, 0x00000078, 0x0000003c, 0x0004003d, 0x0000000d, 0x000000cb, 0x000000ca,
    0x0004003d, 0x0000000d, 0x000000cc, 0x000000b2, 0x00050084, 0x0000000d, 0x000000cd, 0x000000cc,
    0x0000003c, 0x00050080, 0x0000000d, 0x000000ce, 0x000000cb, 0x000000cd, 0x0003003e, 0x000000c9,
    0x000000ce, 0x00050041, 0x00000010, 0x000000d1, 0x000000be, 0x0000004f, 0x0004003d, 0x0000000d,
    0x000000d2, 0x000000d1, 0x00060041, 0x00000046, 0x000000d4, 0x000000c2, 0x00000035, 0x000000d2,
    0x0004003d, 0x0000000e, 0x000000d5, 0x000000d4, 0x0003003e, 0x000000d3, 0x000000d5, 0x0004003d,
    0x0000000d, 0x000000d7, 0x000000c9, 0x0003003e, 0x000000d6, 0x000000d7, 0x00060039, 0x00000011,
    0x000000d8, 0x00000015, 0x000000d3, 0x000000d6, 0x0003003e, 0x000000d0, 0x000000d8, 0x00050041,
    0x00000010, 0x000000da, 0x000000be, 0x00000057, 0x0004003d, 0x0000000d, 0x000000db, 0x000000da,
    0x00060041, 0x00000046, 0x000000dd, 0x000000c2, 0x00000035, 0x000000db, 0x0004003d, 0x0000000e,
    0x000000de, 0x000000dd, 0x0003003e, 0x000000dc, 0x000000de, 0x0004003d, 0x0000000d, 0x000000e0,
    0x000000c9, 0x0003003e, 0x000000df, 0x000000e0, 0x00060039, 0x00000011, 0x000000e1, 0x00000015,
    0x000000dc, 0x000000df, 0x0003003e, 0x000000d9, 0x000000e1, 0x00050041, 0x00000010, 0x000000e3,
    0x000000be, 0x00000060, 0x0004003d, 0x0000000d, 0x000000e4, 0x000000e3, 0x00060041, 0x00000046,
    0x000000e6, 0x000000c2, 0x00000035, 0x000000e4, 0x0004003d, 0x0000000e, 0x000000e7, 0x000000e6,
    0x0003003e, 0x000000e5, 0x000000e7, 0x0004003d, 0x0000000d, 0x000000e9, 0x000000c9, 0x0003003e,
    0x000000e8, 0x000000e9, 0x00060039, 0x00000011, 0x000000ea, 0x00000015, 0x000000e5, 0x000000e8,
    0x0003003e, 0x000000e2, 0x000000ea, 0x00050041, 0x00000007, 0x000000ec, 0x000000d9, 0x0000004f,
    0x0004003d, 0x00000006, 0x000000ed, 0x000000ec, 0x00050041, 0x00000007, 0x000000ee, 0x000000d0,
    0x0000004f, 0x0004003d, 0x00000006, 0x000000ef, 0x000000ee, 0x00050082, 0x00000006, 0x000000f0,
    0x000000ed, 0x000000ef, 0x00050041, 0x00000007, 0x000000f1, 0x000000e2, 0x00000057, 0x0004003d,
    0x00000006, 0x000000f2, 0x000000f1, 0x00050041, 0x00000007, 0x000000f3, 0x000000d0, 0x00000057,
    0x0004003d, 0x00000006, 0x000000f4, 0x000000f3, 0x00050082, 0x00000006, 0x000000f5, 0x000000f2,
    0x000000f4, 0x00050084, 0x00000006, 0x000000f6, 0x000000f0, 0x000000f5, 0x00050041, 0x00000007,
    0x000000f7, 0x000000d9, 0x00000057, 0x0004003d, 0x00000006, 0x000000f8, 0x000000f7, 0x00050041,
    0x00000007, 0x000000f9, 0x000000d0, 0x00000057, 0x0004003d, 0x00000006, 0x000000fa, 0x000000f9,
    0x00050082, 0x00000006, 0x000000fb, 0x000000f8, 0x000000fa, 0x00050041, 0x00000007, 0x000000fc,
    0x000000e2, 0x0000004f, 0x0004003d, 0x00000006, 0x000000fd, 0x000000fc, 0x00050041, 0x00000007,
    0x000000fe, 0x000000d0, 0x0000004f, 0x0004003d, 0x00000006, 0x000000ff, 0x000000fe, 0x00050082,
    0x00000006, 0x00000100, 0x000000fd, 0x000000ff, 0x00050084, 0x00000006, 0x00000101, 0x000000fb,
    0x00000100, 0x00050082, 0x00000006, 0x00000102, 0x000000f6, 0x00000101, 0x0003003e, 0x000000eb,
    0x00000102, 0x0004003d, 0x00000006, 0x00000105, 0x000000eb, 0x000500aa, 0x00000019, 0x00000106,
    0x00000105, 0x00000018, 0x000400a8, 0x00000019, 0x00000107, 0x00000106, 0x000300f7, 0x00000109,
    0x00000000, 0x000400fa, 0x00000107, 0x00000108, 0x00000109, 0x000200f8, 0x00000108, 0x00050041,
    0x0000007a, 0x0000010b, 0x00000078, 0x0000010a, 0x0004003d, 0x0000000d, 0x0000010c, 0x0000010b,
    0x000500ad, 0x00000019, 0x0000010d, 0x0000010c, 0x00000035, 0x0004003d, 0x00000006, 0x0000010e,
    0x000000eb, 0x000500ad, 0x00000019, 0x0000010f, 0x0000010e, 0x00000018, 0x000500a7, 0x00000019,
    0x00000110, 0x0000010d, 0x0000010f, 0x000200f9, 0x00000109, 0x000200f8, 0x00000109, 0x000700f5,
    0x00000019, 0x00000111, 0x00000106, 0x00000005, 0x00000110, 0x00000108, 0x000400a8, 0x00000019,
    0x00000112, 0x00000111, 0x000300f7, 0x00000114, 0x00000000, 0x000400fa, 0x00000112, 0x00000113,
    0x00000114, 0x000200f8, 0x00000113, 0x00050041, 0x0000007a, 0x00000115, 0x00000078, 0x0000010a,
    0x0004003d, 0x0000000d, 0x00000116, 0x00000115, 0x000500b1, 0x00000019, 0x00000117, 0x00000116,
    0x00000035, 0x0004003d, 0x00000006, 0x00000118, 0x000000eb, 0x000500b1, 0x00000019, 0x00000119,
    0x00000118, 0x00000018, 0x000500a7, 0x00000019, 0x0000011a, 0x00000117, 0x00000119, 0x000200f9,
    0x00000114, 0x000200f8, 0x00000114, 0x000700f5, 0x00000019, 0x0000011b, 0x00000111, 0x00000109,
    0x0000011a, 0x00000113, 0x0003003e, 0x00000104, 0x0000011b, 0x0004003d, 0x00000006, 0x0000011c,
    0x000000eb, 0x000500b1, 0x00000019, 0x0000011d, 0x0000011c, 0x00000018, 0x000300f7, 0x0000011f,
    0x00000000, 0x000400fa, 0x0000011d, 0x0000011e, 0x0000011f, 0x000200f8, 0x0000011e, 0x0004003d,
    0x00000011, 0x00000121, 0x000000d9, 0x0003003e, 0x00000120, 0x00000121, 0x0004003d, 0x00000011,
    0x00000122, 0x000000e2, 0x0003003e, 0x000000d9, 0x00000122, 0x0004003d, 0x00000011, 0x00000123,
    0x00000120, 0x0003003e, 0x000000e2, 0x00000123, 0x0004003d, 0x00000006, 0x00000124, 0x000000eb,
    0x0004007e, 0x00000006, 0x00000125, 0x00000124, 0x0003003e, 0x000000eb, 0x00000125, 0x000200f9,
    0x0000011f, 0x000200f8, 0x0000011f, 0x0004003d, 0x00000019, 0x00000126, 0x00000104, 0x000300f7,
    0x00000128, 0x00000000, 0x000400fa, 0x00000126, 0x00000127, 0x00000128, 0x000200f8, 0x00000127,
    0x0003003e, 0x000000eb, 0x00000129, 0x000200f9, 0x00000128, 0x000200f8, 0x00000128, 0x00050041,
    0x00000007, 0x0000012b, 0x000000d9, 0x0000004f, 0x0004003d, 0x00000006, 0x0000012c, 0x0000012b,
    0x00050041, 0x00000007, 0x0000012d, 0x000000d0, 0x0000004f, 0x0004003d, 0x00000006, 0x0000012e,
    0x0000012d, 0x00050082, 0x00000006, 0x0000012f, 0x0000012c, 0x0000012e, 0x0003003e, 0x0000012a,
    0x0000012f, 0x00050041, 0x00000007, 0x00000131, 0x000000d9, 0x00000057, 0x0004003d, 0x00000006,
    0x00000132, 0x00000131, 0x00050041, 0x00000007, 0x00000133, 0x000000d0, 0x00000057, 0x0004003d,
    0x00000006, 0x00000134, 0x00000133, 0x00050082, 0x00000006, 0x00000135, 0x00000132, 0x00000134,
    0x0003003e, 0x00000130, 0x00000135, 0x00050041, 0x00000007, 0x00000137, 0x000000e2, 0x0000004f,
    0x0004003d, 0x00000006, 0x00000138, 0x00000137, 0x00050041, 0x00000007, 0x00000139, 0x000000d0,
    0x0000004f, 0x0004003d, 0x00000006, 0x0000013a, 0x00000139, 0x00050082, 0x00000006, 0x0000013b,
    0x00000138, 0x0000013a, 0x0003003e, 0x00000136, 0x0000013b, 0x00050041, 0x00000007, 0x0000013d,
    0x000000e2, 0x00000057, 0x0004003d, 0x00000006, 0x0000013e, 0x0000013d, 0x00050041, 0x00000007,
    0x0000013f, 0x000000d0, 0x00000057, 0x0004003d, 0x00000006, 0x00000140, 0x0000013f, 0x00050082,
    0x00000006, 0x00000141, 0x0000013e, 0x00000140, 0x0003003e, 0x0000013c, 0x00000141, 0x00050041,
    0x00000007, 0x00000143, 0x000000d9, 0x00000060, 0x0004003d, 0x00000006, 0x00000144, 0x00000143,
    0x00050041, 0x00000007, 0x00000145, 0x000000d0, 0x00000060, 0x0004003d, 0x00000006, 0x00000146,
    0x00000145, 0x00050082, 0x00000006, 0x00000147, 0x00000144, 0x00000146, 0x0003003e, 0x00000142,
    0x00000147, 0x00050041, 0x00000007, 0x00000149, 0x000000e2, 0x00000060, 0x0004003d, 0x00000006,
    0x0000014a, 0x00000149, 0x00050041, 0x00000007, 0x0000014b, 0x000000d0, 0x00000060, 0x0004003d,
    0x00000006, 0x0000014c, 0x0000014b, 0x00050082, 0x00000006, 0x0000014d, 0x0000014a, 0x0000014c,
    0x0003003e, 0x00000148, 0x0000014d, 0x0004003d, 0x00000006, 0x00000150, 0x00000142, 0x0004003d,
    0x00000006, 0x00000151, 0x0000013c, 0x00050084, 0x00000006, 0x00000152, 0x00000150, 0x00000151,
    0x0004003d, 0x00000006, 0x00000153, 0x00000148, 0x0004003d, 0x00000006, 0x00000154, 0x00000130,
    0x00050084, 0x00000006, 0x00000155, 0x00000153, 0x00000154, 0x00050082, 0x00000006, 0x00000156,
    0x00000152, 0x00000155, 0x00050084, 0x00000006, 0x00000157, 0x0000014f, 0x00000156, 0x0004003d,
    0x00000006, 0x00000158, 0x000000eb, 0x00050080, 0x00000006, 0x00000159, 0x00000157, 0x00000158,
    0x0004003d, 0x00000006, 0x0000015b, 0x000000eb, 0x00050084, 0x00000006, 0x0000015c, 0x0000015a,
    0x0000015b, 0x0003003e, 0x0000015d, 0x00000159, 0x0003003e, 0x0000015e, 0x0000015c, 0x00060039,
    0x00000006, 0x0000015f, 0x0000000b, 0x0000015d, 0x0000015e, 0x0003003e, 0x0000014e, 0x0000015f,
    0x0004003d, 0x00000006, 0x00000161, 0x00000148, 0x0004003d, 0x00000006, 0x00000162, 0x0000012a,
    0x00050084, 0x00000006, 0x00000163, 0x00000161, 0x00000162, 0x0004003d, 0x00000006, 0x00000164,
    0x00000142, 0x0004003d, 0x00000006, 0x00000165, 0x00000136, 0x00050084, 0x00000006, 0x00000166,
    0x00000164, 0x00000165, 0x00050082, 0x00000006, 0x00000167, 0x00000163, 0x00000166, 0x00050084,
    0x00000006, 0x00000168, 0x0000014f, 0x00000167, 0x0004003d, 0x00000006, 0x00000169, 0x000000eb,
    0x00050080, 0x00000006, 0x0000016a, 0x00000168, 0x00000169, 0x0004003d, 0x00000006, 0x0000016b,
    0x000000eb, 0x00050084, 0x00000006, 0x0000016c, 0x0000015a, 0x0000016b, 0x0003003e, 0x0000016d,
    0x0000016a, 0x0003003e, 0x0000016e, 0x0000016c, 0x00060039, 0x00000006, 0x0000016f, 0x0000000b,
    0x0000016d, 0x0000016e, 0x0003003e, 0x00000160, 0x0000016f, 0x00050041, 0x00000007, 0x00000171,
    0x000000d0, 0x00000060, 0x0004003d, 0x00000006, 0x00000172, 0x00000171, 0x00050084, 0x00000006,
    0x00000173, 0x0000014f, 0x00000172, 0x0004003d, 0x00000006, 0x00000174, 0x0000014e, 0x00050084,
    0x00000006, 0x00000175, 0x0000015a, 0x00000174, 0x00050041, 0x00000007, 0x00000176, 0x000000d0,
    0x0000004f, 0x0004003d, 0x00000006, 0x00000177, 0x00000176, 0x00050084, 0x00000006, 0x00000178,
    0x00000175, 0x00000177, 0x00050082, 0x00000006, 0x00000179, 0x00000173, 0x00000178, 0x0004003d,
    0x00000006, 0x0000017a, 0x00000160, 0x00050084, 0x00000006, 0x0000017b, 0x0000015a, 0x0000017a,
    0x00050041, 0x00000007, 0x0000017c, 0x000000d0, 0x00000057, 0x0004003d, 0x00000006, 0x0000017d,
    0x0000017c, 0x00050084, 0x00000006, 0x0000017e, 0x0000017b, 0x0000017d, 0x00050082, 0x00000006,
    0x0000017f, 0x00000179, 0x0000017e, 0x0003003e, 0x00000170, 0x0000017f, 0x00050041, 0x00000007,
    0x00000181, 0x000000d0, 0x0000004f, 0x0004003d, 0x00000006, 0x00000182, 0x00000181, 0x00040072,
    0x0000000d, 0x00000183, 0x00000182, 0x00050041, 0x00000007, 0x00000184, 0x000000d0, 0x00000057,
    0x0004003d, 0x00000006, 0x00000185, 0x00000184, 0x00040072, 0x0000000d, 0x00000186, 0x00000185,
    0x00050041, 0x00000007, 0x00000187, 0x000000d9, 0x0000004f, 0x0004003d, 0x00000006, 0x00000188,
    0x00000187, 0x00040072, 0x0000000d, 0x00000189, 0x00000188, 0x00050041, 0x00000007, 0x0000018a,
    0x000000d9, 0x00000057, 0x0004003d, 0x00000006, 0x0000018b, 0x0000018a, 0x00040072, 0x0000000d,
    0x0000018c, 0x0000018b, 0x00070050, 0x0000000e, 0x0000018d, 0x00000183, 0x00000186, 0x00000189,
    0x0000018c, 0x0003003e, 0x00000180, 0x0000018d, 0x00050041, 0x00000007, 0x00000191, 0x000000e2,
    0x0000004f, 0x0004003d, 0x00000006, 0x00000192, 0x00000191, 0x00040072, 0x0000000d, 0x00000193,
    0x00000192, 0x00050041, 0x00000007, 0x00000194, 0x000000e2, 0x00000057, 0x0004003d, 0x00000006,
    0x00000195, 0x00000194, 0x00040072, 0x0000000d, 0x00000196, 0x00000195, 0x00050050, 0x0000018e,
    0x00000197, 0x00000193, 0x00000196, 0x0003003e, 0x00000190, 0x00000197, 0x0004003d, 0x0000000e,
    0x00000199, 0x00000180, 0x0007004f, 0x0000018e, 0x0000019a, 0x00000199, 0x00000199, 0x00000000,
    0x00000001, 0x0004003d, 0x0000000e, 0x0000019b, 0x00000180, 0x0007004f, 0x0000018e, 0x0000019c,
    0x0000019b, 0x0000019b, 0x00000002, 0x00000003, 0x0007000c, 0x0000018e, 0x0000019d, 0x00000001,
    0x00000027, 0x0000019a, 0x0000019c, 0x0004003d, 0x0000018e, 0x0000019e, 0x00000190, 0x0007000c,
    0x0000018e, 0x0000019f, 0x00000001, 0x00000027, 0x0000019d, 0x0000019e, 0x00050041, 0x00000010,
    0x000001a0, 0x00000198, 0x0000004f, 0x00050051, 0x0000000d, 0x000001a1, 0x0000019f, 0x00000000,
    0x0003003e, 0x000001a0, 0x000001a1, 0x00050041, 0x00000010, 0x000001a2, 0x00000198, 0x00000057,
    0x00050051, 0x0000000d, 0x000001a3, 0x0000019f, 0x00000001, 0x0003003e, 0x000001a2, 0x000001a3,
    0x0004003d, 0x0000000e, 0x000001a4, 0x00000180, 0x0007004f, 0x0000018e, 0x000001a5, 0x000001a4,
    0x000001a4, 0x00000000, 0x00000001, 0x0004003d, 0x0000000e, 0x000001a6, 0x00000180, 0x0007004f,
    0x0000018e, 0x000001a7, 0x000001a6, 0x000001a6, 0x00000002, 0x00000003, 0x0007000c, 0x0000018e,
    0x000001a8, 0x00000001, 0x0000002a, 0x000001a5, 0x000001a7, 0x0004003d, 0x0000018e, 0x000001a9,
    0x00000190, 0x0007000c, 0x0000018e, 0x000001aa, 0x00000001, 0x0000002a, 0x000001a8, 0x000001a9,
    0x00050050, 0x0000018e, 0x000001ab, 0x00000071, 0x00000071, 0x00050080, 0x0000018e, 0x000001ac,
    0x000001aa, 0x000001ab, 0x00050041, 0x00000010, 0x000001ad, 0x00000198, 0x00000060, 0x00050051,
    0x0000000d, 0x000001ae, 0x000001ac, 0x00000000, 0x0003003e, 0x000001ad, 0x000001ae, 0x00050041,
    0x00000010, 0x000001af, 0x00000198, 0x0000004a, 0x00050051, 0x0000000d, 0x000001b0, 0x000001ac,
    0x00000001, 0x0003003e, 0x000001af, 0x000001b0, 0x0004003d, 0x0000000e, 0x000001b1, 0x00000198,
    0x00050041, 0x000001b3, 0x000001b4, 0x00000078, 0x00000035, 0x0004003d, 0x00000075, 0x000001b5,
    0x000001b4, 0x0004006e, 0x0000018e, 0x000001b6, 0x000001b5, 0x00050041, 0x000001b3, 0x000001b7,
    0x00000078, 0x00000035, 0x0004003d, 0x00000075, 0x000001b8, 0x000001b7, 0x0004006e, 0x0000018e,
    0x000001b9, 0x000001b8, 0x00050051, 0x0000000d, 0x000001ba, 0x000001b6, 0x00000000, 0x00050051,
    0x0000000d, 0x000001bb, 0x000001b6, 0x00000001, 0x00050051, 0x0000000d, 0x000001bc, 0x000001b9,
    0x00000000, 0x00050051, 0x0000000d, 0x000001bd, 0x000001b9, 0x00000001, 0x00070050, 0x0000000e,
    0x000001be, 0x000001ba, 0x000001bb, 0x000001bc, 0x000001bd, 0x0008000c, 0x0000000e, 0x000001bf,
    0x00000001, 0x0000002d, 0x000001b1, 0x000001b2, 0x000001be, 0x0003003e, 0x00000198, 0x000001bf,
    0x0004003d, 0x00000019, 0x000001c0, 0x00000104, 0x000300f7, 0x000001c2, 0x00000000, 0x000400fa,
    0x000001c0, 0x000001c1, 0x000001c2, 0x000200f8, 0x000001c1, 0x0003003e, 0x00000198, 0x000001b2,
    0x000200f9, 0x000001c2, 0x000200f8, 0x000001c2, 0x0004003d, 0x0000000d, 0x000001c5, 0x000001c4,
    0x0003003e, 0x000001c3, 0x000001c5, 0x0004003d, 0x0000000d, 0x000001c8, 0x000001c3, 0x000500c7,
    0x0000000d, 0x000001c9, 0x000001c8, 0x00000071, 0x000500ab, 0x00000019, 0x000001ca, 0x000001c9,
    0x00000035, 0x000300f7, 0x000001cd, 0x00000000, 0x000400fa, 0x000001ca, 0x000001cc, 0x000001d0,
    0x000200f8, 0x000001cc, 0x00050041, 0x00000010, 0x000001ce, 0x00000198, 0x00000060, 0x0004003d,
    0x0000000d, 0x000001cf, 0x000001ce, 0x0003003e, 0x000001cb, 0x000001cf, 0x000200f9, 0x000001cd,
    0x000200f8, 0x000001d0, 0x00050041, 0x00000010, 0x000001d1, 0x00000198, 0x0000004f, 0x0004003d,
    0x0000000d, 0x000001d2, 0x000001d1, 0x0003003e, 0x000001cb, 0x000001d2, 0x000200f9, 0x000001cd,
    0x000200f8, 0x000001cd, 0x0004003d, 0x0000000d, 0x000001d3, 0x000001cb, 0x0004006f, 0x00000074,
    0x000001d4, 0x000001d3, 0x0004003d, 0x0000000d, 0x000001d5, 0x000001c3, 0x000500c7, 0x0000000d,
    0x000001d6, 0x000001d5, 0x00000088, 0x000500ab, 0x00000019, 0x000001d7, 0x000001d6, 0x00000035,
    0x000300f7, 0x000001da, 0x00000000, 0x000400fa, 0x000001d7, 0x000001d9, 0x000001dd, 0x000200f8,
    0x000001d9, 0x00050041, 0x00000010, 0x000001db, 0x00000198, 0x0000004a, 0x0004003d, 0x0000000d,
    0x000001dc, 0x000001db, 0x0003003e, 0x000001d8, 0x000001dc, 0x000200f9, 0x000001da, 0x000200f8,
    0x000001dd, 0x00050041, 0x00000010, 0x000001de, 0x00000198, 0x00000057, 0x0004003d, 0x0000000d,
    0x000001df, 0x000001de, 0x0003003e, 0x000001d8, 0x000001df, 0x000200f9, 0x000001da, 0x000200f8,
    0x000001da, 0x0004003d, 0x0000000d, 0x000001e0, 0x000001d8, 0x0004006f, 0x00000074, 0x000001e1,
    0x000001e0, 0x00050050, 0x00000075, 0x000001e2, 0x000001d4, 0x000001e1, 0x0003003e, 0x000001c7,
    0x000001e2, 0x0004003d, 0x00000075, 0x000001e8, 0x000001c7, 0x00050041, 0x000001b3, 0x000001e9,
    0x00000078, 0x00000035, 0x0004003d, 0x00000075, 0x000001ea, 0x000001e9, 0x00050088, 0x00000075,
    0x000001eb, 0x000001e8, 0x000001ea, 0x0005008e, 0x00000075, 0x000001ed, 0x000001eb, 0x000001ec,
    0x00050050, 0x00000075, 0x000001ef, 0x000001ee, 0x000001ee, 0x00050083, 0x00000075, 0x000001f0,
    0x000001ed, 0x000001ef, 0x00050051, 0x00000074, 0x000001f2, 0x000001f0, 0x00000000, 0x00050051,
    0x00000074, 0x000001f3, 0x000001f0, 0x00000001, 0x00070050, 0x000001e3, 0x000001f4, 0x000001f2,
    0x000001f3, 0x000001f1, 0x000001ee, 0x00050041, 0x000001f5, 0x000001f6, 0x000001e7, 0x00000035,
    0x0003003e, 0x000001f6, 0x000001f4, 0x0004003d, 0x0000018e, 0x000001f9, 0x00000190, 0x0003003e,
    0x000001f8, 0x000001f9, 0x00050041, 0x00000010, 0x000001fd, 0x000000be, 0x0000004a, 0x0004003d,
    0x0000000d, 0x000001fe, 0x000001fd, 0x0004007c, 0x00000049, 0x000001ff, 0x000001fe, 0x000500c7,
    0x00000049, 0x00000201, 0x000001ff, 0x00000200, 0x00050050, 0x000001fa, 0x00000202, 0x00000201,
    0x0000004a, 0x0003003e, 0x000001fc, 0x00000202, 0x0004003d, 0x0000000e, 0x00000205, 0x00000180,
    0x0003003e, 0x00000204, 0x00000205, 0x0004003d, 0x00000006, 0x00000209, 0x0000014e, 0x0004007c,
    0x0000001e, 0x0000020a, 0x00000209, 0x000500c7, 0x0000001e, 0x0000020c, 0x0000020a, 0x0000020b,
    0x00040071, 0x00000049, 0x0000020d, 0x0000020c, 0x0004003d, 0x00000006, 0x0000020e, 0x00000160,
    0x0004007c, 0x0000001e, 0x0000020f, 0x0000020e, 0x000500c7, 0x0000001e, 0x00000210, 0x0000020f,
    0x0000020b, 0x00040071, 0x00000049, 0x00000211, 0x00000210, 0x0004003d, 0x00000006, 0x00000212,
    0x00000170, 0x0004007c, 0x0000001e, 0x00000213, 0x00000212, 0x000500c7, 0x0000001e, 0x00000214,
    0x00000213, 0x0000020b, 0x00040071, 0x00000049, 0x00000215, 0x00000214, 0x00070050, 0x00000206,
    0x00000216, 0x0000020d, 0x00000211, 0x00000215, 0x0000004f, 0x0003003e, 0x00000208, 0x00000216,
    0x000100fd, 0x00010038, 0x00050036, 0x00000006, 0x0000000b, 0x00000000, 0x00000008, 0x00030037,
    0x00000007, 0x00000009, 0x00030037, 0x00000007, 0x0000000a, 0x000200f8, 0x0000000c, 0x0004003d,
    0x00000006, 0x00000017, 0x00000009, 0x000500af, 0x00000019, 0x0000001a, 0x00000017, 0x00000018,
    0x000300f7, 0x0000001c, 0x00000000, 0x000400fa, 0x0000001a, 0x0000001b, 0x0000001c, 0x000200f8,
    0x0000001b, 0x0004003d, 0x00000006, 0x0000001d, 0x00000009, 0x0004007c, 0x0000001e, 0x0000001f,
    0x0000001d, 0x0004003d, 0x00000006, 0x00000020, 0x0000000a, 0x0004007c, 0x0000001e, 0x00000021,
    0x00000020, 0x00050086, 0x0000001e, 0x00000022, 0x0000001f, 0x00000021, 0x0004007c, 0x00000006,
    0x00000023, 0x00000022, 0x000200fe, 0x00000023, 0x000200f8, 0x0000001c, 0x0004003d, 0x00000006,
    0x00000025, 0x00000009, 0x0004007e, 0x00000006, 0x00000026, 0x00000025, 0x0004007c, 0x0000001e,
    0x00000027, 0x00000026, 0x0004003d, 0x00000006, 0x00000028, 0x0000000a, 0x0004007c, 0x0000001e,
    0x00000029, 0x00000028, 0x00050080, 0x0000001e, 0x0000002a, 0x00000027, 0x00000029, 0x00050082,
    0x0000001e, 0x0000002c, 0x0000002a, 0x0000002b, 0x0004003d, 0x00000006, 0x0000002d, 0x0000000a,
    0x0004007c, 0x0000001e, 0x0000002e, 0x0000002d, 0x00050086, 0x0000001e, 0x0000002f, 0x0000002c,
    0x0000002e, 0x0004007c, 0x00000006, 0x00000030, 0x0000002f, 0x0004007e, 0x00000006, 0x00000031,
    0x00000030, 0x000200fe, 0x00000031, 0x00010038, 0x00050036, 0x00000011, 0x00000015, 0x00000000,
    0x00000012, 0x00030037, 0x0000000f, 0x00000013, 0x00030037, 0x00000010, 0x00000014, 0x000200f8,
    0x00000016, 0x0004003b, 0x00000010, 0x00000034, 0x00000007, 0x0004003b, 0x0000000f, 0x0000003e,
    0x00000007, 0x0004003b, 0x0000004c, 0x0000004d, 0x00000007, 0x0004003b, 0x00000007, 0x00000073,
    0x00000007, 0x0004003b, 0x00000007, 0x00000089, 0x00000007, 0x0004003b, 0x00000007, 0x0000008a,
    0x00000007, 0x0004003b, 0x00000007, 0x0000009b, 0x00000007, 0x0004003b, 0x00000007, 0x0000009c,
    0x00000007, 0x0004003b, 0x00000007, 0x000000a5, 0x00000007, 0x0004003b, 0x00000007, 0x000000a6,
    0x00000007, 0x0003003e, 0x00000034, 0x00000035, 0x000200f9, 0x00000036, 0x000200f8, 0x00000036,
    0x000400f6, 0x00000038, 0x00000039, 0x00000000, 0x000200f9, 0x0000003a, 0x000200f8, 0x0000003a,
    0x0004003d, 0x0000000d, 0x0000003b, 0x00000034, 0x000500b1, 0x00000019, 0x0000003d, 0x0000003b,
    0x0000003c, 0x000400fa, 0x0000003d, 0x00000037, 0x00000038, 0x000200f8, 0x00000037, 0x0004003d,
    0x0000000d, 0x00000043, 0x00000014, 0x0004003d, 0x0000000d, 0x00000044, 0x00000034, 0x00050080,
    0x0000000d, 0x00000045, 0x00000043, 0x00000044, 0x00060041, 0x00000046, 0x00000047, 0x00000042,
    0x00000035, 0x00000045, 0x0004003d, 0x0000000e, 0x00000048, 0x00000047, 0x0003003e, 0x0000003e,
    0x00000048, 0x0004003d, 0x0000000d, 0x0000004e, 0x00000034, 0x00050041, 0x00000010, 0x00000050,
    0x0000003e, 0x0000004f, 0x0004003d, 0x0000000d, 0x00000051, 0x00000050, 0x00040072, 0x00000006,
    0x00000052, 0x00000051, 0x00050041, 0x00000010, 0x00000053, 0x00000013, 0x0000004f, 0x0004003d,
    0x0000000d, 0x00000054, 0x00000053, 0x00040072, 0x00000006, 0x00000055, 0x00000054, 0x00050084,
    0x00000006, 0x00000056, 0x00000052, 0x00000055, 0x00050041, 0x00000010, 0x00000058, 0x0000003e,
    0x00000057, 0x0004003d, 0x0000000d, 0x00000059, 0x00000058, 0x00040072, 0x00000006, 0x0000005a,
    0x00000059, 0x00050041, 0x00000010, 0x0000005b, 0x00000013, 0x00000057, 0x0004003d, 0x0000000d,
    0x0000005c, 0x0000005b, 0x00040072, 0x00000006, 0x0000005d, 0x0000005c, 0x00050084, 0x00000006,
    0x0000005e, 0x0000005a, 0x0000005d, 0x00050080, 0x00000006, 0x0000005f, 0x00000056, 0x0000005e,
    0x00050041, 0x00000010, 0x00000061, 0x0000003e, 0x00000060, 0x0004003d, 0x0000000d, 0x00000062,
    0x00000061, 0x00040072, 0x00000006, 0x00000063, 0x00000062, 0x00050041, 0x00000010, 0x00000064,
    0x00000013, 0x00000060, 0x0004003d, 0x0000000d, 0x00000065, 0x00000064, 0x00040072, 0x00000006,
    0x00000066, 0x00000065, 0x00050084, 0x00000006, 0x00000067, 0x00000063, 0x00000066, 0x00050080,
    0x00000006, 0x00000068, 0x0000005f, 0x00000067, 0x000500c3, 0x00000006, 0x0000006a, 0x00000068,
    0x00000069, 0x00050041, 0x00000010, 0x0000006b, 0x0000003e, 0x0000004a, 0x0004003d, 0x0000000d,
    0x0000006c, 0x0000006b, 0x00040072, 0x00000006, 0x0000006d, 0x0000006c, 0x00050080, 0x00000006,
    0x0000006e, 0x0000006a, 0x0000006d, 0x00050041, 0x00000007, 0x0000006f, 0x0000004d, 0x0000004e,
    0x0003003e, 0x0000006f, 0x0000006e, 0x000200f9, 0x00000039, 0x000200f8, 0x00000039, 0x0004003d,
    0x0000000d, 0x00000070, 0x00000034, 0x00050080, 0x0000000d, 0x00000072, 0x00000070, 0x00000071,
    0x0003003e, 0x00000034, 0x00000072, 0x000200f9, 0x00000036, 0x000200f8, 0x00000038, 0x00050041,
    0x0000007a, 0x0000007b, 0x00000078, 0x00000079, 0x0004003d, 0x0000000d, 0x0000007c, 0x0000007b,
    0x00040072, 0x00000006, 0x0000007d, 0x0000007c, 0x0003003e, 0x00000073, 0x0000007d, 0x00050041,
    0x0000007a, 0x0000007f, 0x00000078, 0x0000007e, 0x0004003d, 0x0000000d, 0x00000080, 0x0000007f,
    0x00040072, 0x00000006, 0x00000081, 0x00000080, 0x00050084, 0x00000006, 0x00000083, 0x00000081,
    0x00000082, 0x0004003d, 0x00000006, 0x00000084, 0x00000073, 0x00050041, 0x00000007, 0x00000085,
    0x0000004d, 0x00000035, 0x0004003d, 0x00000006, 0x00000086, 0x00000085, 0x00050084, 0x00000006,
    0x00000087, 0x00000084, 0x00000086, 0x0003003e, 0x00000089, 0x00000087, 0x00050041, 0x00000007,
    0x0000008b, 0x0000004d, 0x00000088, 0x0004003d, 0x00000006, 0x0000008c, 0x0000008b, 0x0003003e,
    0x0000008a, 0x0000008c, 0x00060039, 0x00000006, 0x0000008d, 0x0000000b, 0x00000089, 0x0000008a,
    0x00050080, 0x00000006, 0x0000008e, 0x00000083, 0x0000008d, 0x00050080, 0x00000006, 0x0000008f,
    0x0000008e, 0x00000082, 0x000500c3, 0x00000006, 0x00000091, 0x0000008f, 0x00000090, 0x00050041,
    0x0000007a, 0x00000093, 0x00000078, 0x00000092, 0x0004003d, 0x0000000d, 0x00000094, 0x00000093,
    0x00040072, 0x00000006, 0x00000095, 0x00000094, 0x00050084, 0x00000006, 0x00000096, 0x00000095,
    0x00000082, 0x0004003d, 0x00000006, 0x00000097, 0x00000073, 0x00050041, 0x00000007, 0x00000098,
    0x0000004d, 0x00000071, 0x0004003d, 0x00000006, 0x00000099, 0x00000098, 0x00050084, 0x00000006,
    0x0000009a, 0x00000097, 0x00000099, 0x0003003e, 0x0000009b, 0x0000009a, 0x00050041, 0x00000007,
    0x0000009d, 0x0000004d, 0x00000088, 0x0004003d, 0x00000006, 0x0000009e, 0x0000009d, 0x0003003e,
    0x0000009c, 0x0000009e, 0x00060039, 0x00000006, 0x0000009f, 0x0000000b, 0x0000009b, 0x0000009c,
    0x00050082, 0x00000006, 0x000000a0, 0x00000096, 0x0000009f, 0x00050080, 0x00000006, 0x000000a1,
    0x000000a0, 0x00000082, 0x000500c3, 0x00000006, 0x000000a2, 0x000000a1, 0x00000090, 0x0003003e,
    0x000000a5, 0x000000a4, 0x00050041, 0x00000007, 0x000000a7, 0x0000004d, 0x00000088, 0x0004003d,
    0x00000006, 0x000000a8, 0x000000a7, 0x0003003e, 0x000000a6, 0x000000a8, 0x00060039, 0x00000006,
    0x000000a9, 0x0000000b, 0x000000a5, 0x000000a6, 0x00050082, 0x00000006, 0x000000aa, 0x000000a3,
    0x000000a9, 0x00060050, 0x00000011, 0x000000ab, 0x00000091, 0x000000a2, 0x000000aa, 0x000200fe,
    0x000000ab, 0x00010038,
};
// @@SPIRV-END@@
TB_API struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
    VkSurfaceKHR surf; VkSwapchainKHR sc; uint32_t sc_n, sc_w, sc_h; VkImage sc_img[8];
    VkPhysicalDeviceMemoryProperties mp;
    VkImage img; VkImageView view; VkDeviceMemory imem; VkRenderPass rp; VkFramebuffer fb;
    VkPipelineLayout pl; VkPipeline pipe, pipe3; VkDescriptorSet ds;   // pipe — 2D, pipe3 — сетки с глубиной
    VkImage zimg; VkImageView zview; VkDeviceMemory zmem; int zfresh;  // глубина (D32_SFLOAT)
    VkBuffer gpbuf;                                                      // плоскости глубины (tb_gplane)
    VkPipeline pipem; VkBuffer mbuf, grbuf; VkDeviceMemory mmem;         // 3D на видеокарте: конвейер, сетки, записи draw_mesh
    VkBuffer pbuf; VkDeviceMemory pmem;   // точки всех картинок
    VkBuffer ibuf[TB_GSLOTS], fbuf, rbuf; void *imap[TB_GSLOTS], *rmap;
    VkCommandPool pool; VkCommandBuffer cb[TB_GSLOTS]; VkFence fence[TB_GSLOTS];
    VkSemaphore sem_get[TB_GSLOTS], sem_done[TB_GSLOTS];
} tb_vk;
TB_API int tb_vk_memtype(uint32_t bits, VkFlags want) {
    for (uint32_t i = 0; i < tb_vk.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (tb_vk.mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
// буфер в памяти, которую видят и процессор, и видеокарта; map — её адрес для процессора
TB_API int tb_vk_buffer_m(VkDeviceSize size, VkFlags usage, VkBuffer *buf, void **map, VkDeviceMemory *memo);
TB_API int tb_vk_buffer(VkDeviceSize size, VkFlags usage, VkBuffer *buf, void **map) { VkDeviceMemory m; return tb_vk_buffer_m(size, usage, buf, map, &m); }
TB_API int tb_vk_buffer_m(VkDeviceSize size, VkFlags usage, VkBuffer *buf, void **map, VkDeviceMemory *memo) {
    VkBufferCreateInfo bi = { 12, 0, 0, size, usage, 0, 0, 0 };
    if (tbvk_CreateBuffer(tb_vk.dev, &bi, 0, buf)) return 0;
    VkMemoryRequirements mr; tbvk_GetBufferMemoryRequirements(tb_vk.dev, *buf, &mr);
    int t = tb_vk_memtype(mr.memoryTypeBits, 2 | 4);   // HOST_VISIBLE | HOST_COHERENT
    if (t < 0) return 0;
    VkMemoryAllocateInfo ai = { 5, 0, mr.size, (uint32_t)t };
    VkDeviceMemory mem;
    if (tbvk_AllocateMemory(tb_vk.dev, &ai, 0, &mem) || tbvk_BindBufferMemory(tb_vk.dev, *buf, mem, 0)) return 0;
    *memo = mem;
    return tbvk_MapMemory(tb_vk.dev, mem, 0, size, 0, map) == 0;
}
// картинки поменялись (load_image): все точки — в новый буфер видеокарты
TB_API int tb_vk_images(void) {
    if (tb_vk.pbuf) {
        tbvk_DeviceWaitIdle(tb_vk.dev);
        tbvk_DestroyBuffer(tb_vk.dev, tb_vk.pbuf, 0);
        tbvk_FreeMemory(tb_vk.dev, tb_vk.pmem, 0);
        tb_vk.pbuf = 0;
    }
    VkDeviceSize sz = tb_imgpx_n * 4 > 16 ? tb_imgpx_n * 4 : 16;
    void *map;
    if (!tb_vk_buffer_m(sz, 0x20, &tb_vk.pbuf, &map, &tb_vk.pmem)) return 0;   // STORAGE_BUFFER
    if (tb_imgpx_n) memcpy(map, tb_imgpx, tb_imgpx_n * 4);
    VkDescriptorBufferInfo dbi = { tb_vk.pbuf, 0, sz };
    VkWriteDescriptorSet wds = { 35, 0, tb_vk.ds, 1, 0, 1, 7, 0, &dbi, 0 };
    tbvk_UpdateDescriptorSets(tb_vk.dev, 1, &wds, 0, 0);
    tb_img_dirty = 0;
    return 1;
}
// сетки поменялись (mesh_add): все сетки — в новый буфер видеокарты
TB_API int tb_vk_meshes(void) {
    if (tb_vk.mbuf) {
        tbvk_DeviceWaitIdle(tb_vk.dev);
        tbvk_DestroyBuffer(tb_vk.dev, tb_vk.mbuf, 0);
        tbvk_FreeMemory(tb_vk.dev, tb_vk.mmem, 0);
        tb_vk.mbuf = 0;
    }
    long long n = tb_mesh_pack(0);
    VkDeviceSize sz = n > 0 ? (VkDeviceSize)n * 16 : 16;
    void *map;
    if (!tb_vk_buffer_m(sz, 0x20, &tb_vk.mbuf, &map, &tb_vk.mmem)) return 0;   // STORAGE_BUFFER
    if (n > 0) tb_mesh_pack((int32_t *)map);
    VkDescriptorBufferInfo dbi = { tb_vk.mbuf, 0, sz };
    VkWriteDescriptorSet wds = { 35, 0, tb_vk.ds, 3, 0, 1, 7, 0, &dbi, 0 };
    tbvk_UpdateDescriptorSets(tb_vk.dev, 1, &wds, 0, 0);
    tb_mesh_gdirty = 0;
    return 1;
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
    uint32_t np = 0, pm[16], mode = 2;   // FIFO: кадр вместе с обновлением экрана (есть всегда)
    tbvk_GetPhysicalDeviceSurfacePresentModesKHR(tb_vk.pd, tb_vk.surf, &np, 0); if (np > 16) np = 16;
    tbvk_GetPhysicalDeviceSurfacePresentModesKHR(tb_vk.pd, tb_vk.surf, &np, pm);
    if (tb_gpu_nosync) {   // не ждать экран (IMMEDIATE, иначе MAILBOX)
        for (uint32_t i = 0; i < np; i++) if (pm[i] == 1) mode = 1;
        for (uint32_t i = 0; i < np; i++) if (pm[i] == 0) mode = 0;
    }
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
// свой кадр на видеокарте (B8G8R8A8: как tb_px в памяти): живёт между кадрами, как tb_px
TB_API int tb_vk_target(void) {
    VkImageCreateInfo ii = { 14, 0, 0, 1, 44, { (uint32_t)tb_gw, (uint32_t)tb_gh, 1 }, 1, 1, 1, 0, 16 | 2 | 1, 0, 0, 0, 0 };
    if (tbvk_CreateImage(tb_vk.dev, &ii, 0, &tb_vk.img)) return 0;
    VkMemoryRequirements mr; tbvk_GetImageMemoryRequirements(tb_vk.dev, tb_vk.img, &mr);
    int mt = tb_vk_memtype(mr.memoryTypeBits, 1);
    if (mt < 0) mt = tb_vk_memtype(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai = { 5, 0, mr.size, (uint32_t)mt };
    if (mt < 0 || tbvk_AllocateMemory(tb_vk.dev, &mai, 0, &tb_vk.imem) || tbvk_BindImageMemory(tb_vk.dev, tb_vk.img, tb_vk.imem, 0)) return 0;
    VkImageViewCreateInfo vci = { 15, 0, 0, tb_vk.img, 1, 44, { 0, 0, 0, 0 }, { 1, 0, 1, 0, 1 } };
    if (tbvk_CreateImageView(tb_vk.dev, &vci, 0, &tb_vk.view)) return 0;
    // глубина для 3D: D32_SFLOAT, только на видеокарте
    VkImageCreateInfo zi = { 14, 0, 0, 1, 126, { (uint32_t)tb_gw, (uint32_t)tb_gh, 1 }, 1, 1, 1, 0, 0x20, 0, 0, 0, 0 };   // DEPTH_STENCIL_ATTACHMENT
    if (tbvk_CreateImage(tb_vk.dev, &zi, 0, &tb_vk.zimg)) return 0;
    tbvk_GetImageMemoryRequirements(tb_vk.dev, tb_vk.zimg, &mr);
    mt = tb_vk_memtype(mr.memoryTypeBits, 1);
    if (mt < 0) mt = tb_vk_memtype(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo zai = { 5, 0, mr.size, (uint32_t)mt };
    if (mt < 0 || tbvk_AllocateMemory(tb_vk.dev, &zai, 0, &tb_vk.zmem) || tbvk_BindImageMemory(tb_vk.dev, tb_vk.zimg, tb_vk.zmem, 0)) return 0;
    VkImageViewCreateInfo zvci = { 15, 0, 0, tb_vk.zimg, 1, 126, { 0, 0, 0, 0 }, { 2, 0, 1, 0, 1 } };   // DEPTH
    if (tbvk_CreateImageView(tb_vk.dev, &zvci, 0, &tb_vk.zview)) return 0;
    tb_vk.zfresh = 1;
    VkImageView views[2] = { tb_vk.view, tb_vk.zview };
    VkFramebufferCreateInfo fci = { 37, 0, 0, tb_vk.rp, 2, views, (uint32_t)tb_gw, (uint32_t)tb_gh, 1 };
    return tbvk_CreateFramebuffer(tb_vk.dev, &fci, 0, &tb_vk.fb) == 0;
}
// TB_GPU_CHECK: буфер, куда кадр читается обратно для сверки
TB_API VkDeviceMemory tb_vk_rmem = 0;
TB_API int tb_vk_readback(void) {
    if (!tb_gpu_check) return 1;
    if (tb_vk.rbuf) { tbvk_DestroyBuffer(tb_vk.dev, tb_vk.rbuf, 0); tbvk_FreeMemory(tb_vk.dev, tb_vk_rmem, 0); tb_vk.rbuf = 0; }
    if (!tb_vk_buffer_m((VkDeviceSize)tb_gw * tb_gh * 4, 2, &tb_vk.rbuf, &tb_vk.rmap, &tb_vk_rmem)) return 0;   // TRANSFER_DST
    tb_gpu_back = (uint32_t *)tb_vk.rmap;
    return 1;
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
    char what[400]; snprintf(what, sizeof what, "видеокарта (Vulkan, %s)", (const char *)props + 20);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { 2, 0, 0, tb_vk.qf, 1, &prio };
    const char *dext[1] = { "VK_KHR_swapchain" };
    // 3D на видеокарте считается в 64-битных целых (shaderInt64, место 40 в VkPhysicalDeviceFeatures): нет их — сетки на процессоре
    VkBool32 feat[55] = { 0 }, want[55] = { 0 };
    tbvk_GetPhysicalDeviceFeatures(tb_vk.pd, feat);
    want[40] = feat[40];
    VkDeviceCreateInfo dci = { 3, 0, 0, 1, &qci, 0, 0, 1, dext, want };
    if (tbvk_CreateDevice(tb_vk.pd, &dci, 0, &tb_vk.dev)) return 0;
    tbvk_GetDeviceQueue(tb_vk.dev, tb_vk.qf, 0, &tb_vk.q);
    tbvk_GetPhysicalDeviceMemoryProperties(tb_vk.pd, &tb_vk.mp);
    VkFormatProperties fp; tbvk_GetPhysicalDeviceFormatProperties(tb_vk.pd, 44, &fp);
    if ((fp.optimalTilingFeatures & (0x80 | 0x400)) != (0x80 | 0x400)) return 0;   // COLOR_ATTACHMENT, BLIT_SRC
    tbvk_GetPhysicalDeviceFormatProperties(tb_vk.pd, 126, &fp);
    if (!(fp.optimalTilingFeatures & 0x200)) return 0;   // D32_SFLOAT как глубина (DEPTH_STENCIL_ATTACHMENT)
    if (!tb_vk_swapchain()) return 0;
    // проход рисования: старый кадр сохраняется (LOAD), рисуем поверх — как на процессоре
    // глубина тоже сохраняется между проходами кадра; стирается в начале кадра (tb_gzclr)
    VkAttachmentDescription ad[2] = { { 0, 44, 1, 0, 0, 2, 1, 2, 2 },     // LOAD, STORE, COLOR_ATTACHMENT_OPTIMAL
                                      { 0, 126, 1, 0, 0, 2, 1, 3, 3 } };  // D32_SFLOAT, DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    VkAttachmentReference ar = { 0, 2 }, zr = { 1, 3 };
    VkSubpassDescription sd = { 0, 0, 0, 0, 1, &ar, 0, &zr, 0, 0 };
    VkSubpassDependency dep = { 0xFFFFFFFFu, 0, 0x400 | 0x1000 | 0x100 | 0x200, 0x400 | 0x100 | 0x200,
        0x100 | 0x800 | 0x1000 | 0x400, 0x80 | 0x100 | 0x200 | 0x400, 0 };
    VkRenderPassCreateInfo rci = { 38, 0, 0, 2, ad, 1, &sd, 1, &dep };
    if (tbvk_CreateRenderPass(tb_vk.dev, &rci, 0, &tb_vk.rp)) return 0;
    if (!tb_vk_target()) return 0;
    // шрифт — для шейдера букв (uniform buffer, 161 буква по 16 байт)
    void *fmap;
    if (!tb_vk_buffer(sizeof tb_font, 0x10, &tb_vk.fbuf, &fmap)) return 0;
    memcpy(fmap, tb_font, sizeof tb_font);
    VkDescriptorSetLayoutBinding lb[5] = { { 0, 6, 1, 0x10, 0 }, { 1, 7, 1, 0x10, 0 }, { 2, 7, 1, 0x10, 0 },   // UNIFORM, STORAGE ×2; FRAGMENT
                                           { 3, 7, 1, 1, 0 }, { 4, 7, 1, 1, 0 } };   // сетки и записи draw_mesh: VERTEX
    VkDescriptorSetLayoutCreateInfo lci = { 32, 0, 0, 5, lb };
    VkDescriptorSetLayout dsl;
    if (tbvk_CreateDescriptorSetLayout(tb_vk.dev, &lci, 0, &dsl)) return 0;
    VkDescriptorPoolSize ps[2] = { { 6, 1 }, { 7, 4 } };
    VkDescriptorPoolCreateInfo pci = { 33, 0, 0, 1, 2, ps };
    VkDescriptorPool dp;
    if (tbvk_CreateDescriptorPool(tb_vk.dev, &pci, 0, &dp)) return 0;
    VkDescriptorSetAllocateInfo dai = { 34, 0, dp, 1, &dsl };
    if (tbvk_AllocateDescriptorSets(tb_vk.dev, &dai, &tb_vk.ds)) return 0;
    VkDescriptorBufferInfo dbi = { tb_vk.fbuf, 0, sizeof tb_font };
    VkWriteDescriptorSet wds = { 35, 0, tb_vk.ds, 0, 0, 1, 6, 0, &dbi, 0 };
    tbvk_UpdateDescriptorSets(tb_vk.dev, 1, &wds, 0, 0);
    // плоскости глубины 3D: по TB_GI_CAP на каждый набор, программа пишет их прямо сюда
    void *gmap;
    VkDeviceSize gsz = sizeof(tb_gplane_t) * TB_GI_CAP * TB_GSLOTS;
    if (!tb_vk_buffer(gsz, 0x20, &tb_vk.gpbuf, &gmap)) return 0;   // STORAGE_BUFFER
    tb_gplane = (tb_gplane_t *)gmap;
    VkDescriptorBufferInfo gbi = { tb_vk.gpbuf, 0, gsz };
    VkWriteDescriptorSet gwds = { 35, 0, tb_vk.ds, 2, 0, 1, 7, 0, &gbi, 0 };
    tbvk_UpdateDescriptorSets(tb_vk.dev, 1, &gwds, 0, 0);
    if (!tb_vk_images()) return 0;
    // записи draw_mesh: по TB_GR_CAP на набор, программа пишет их прямо сюда
    void *rmap;
    VkDeviceSize rsz = sizeof(tb_grec) * TB_GR_CAP * TB_GSLOTS;
    if (!tb_vk_buffer(rsz, 0x20, &tb_vk.grbuf, &rmap)) return 0;
    tb_grec_p = (tb_grec *)rmap;
    VkDescriptorBufferInfo rbi = { tb_vk.grbuf, 0, rsz };
    VkWriteDescriptorSet rwds = { 35, 0, tb_vk.ds, 4, 0, 1, 7, 0, &rbi, 0 };
    tbvk_UpdateDescriptorSets(tb_vk.dev, 1, &rwds, 0, 0);
    if (!tb_vk_meshes()) return 0;
    VkPushConstantRange pcr = { 1, 0, sizeof(tb_gpc) };   // VERTEX: размер окна (+ для сеток: где сетка, записи, экран)
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
    VkViewport vp = { 0, 0, (float)tb_gw, (float)tb_gh, 0, 1 };   // меняется в каждом кадре (окно тянут мышью)
    uint32_t dyn[2] = { 0, 1 };   // VIEWPORT, SCISSOR
    VkPipelineDynamicStateCreateInfo dys = { 27, 0, 0, 2, dyn };
    VkRect2D sci = { { 0, 0 }, { (uint32_t)tb_gw, (uint32_t)tb_gh } };
    VkPipelineViewportStateCreateInfo vps = { 22, 0, 0, 1, &vp, 1, &sci };
    VkPipelineRasterizationStateCreateInfo rs = { 23, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { 24, 0, 0, 1, 0, 0, 0, 0, 0 };
    VkPipelineColorBlendAttachmentState cba = { 1, 1, 7, 0, 1, 0, 0, 0xF };   // смешивание: ONE, ONE_MINUS_SRC_ALPHA (обычные фигуры дают a = 1 и просто кладут цвет)
    VkPipelineColorBlendStateCreateInfo cbs = { 26, 0, 0, 0, 0, 1, &cba, { 0, 0, 0, 0 } };
    // два конвейера: 2D — глубину не трогает и не проверяет; 3D (сетки) — ближе (LESS) рисуется и пишет глубину
    VkPipelineDepthStencilStateCreateInfo z2 = { 25, 0, 0, 0, 0, 7, 0, 0, { 0 }, { 0 }, 0, 1 };
    VkPipelineDepthStencilStateCreateInfo z3 = { 25, 0, 0, 1, 1, 1, 0, 0, { 0 }, { 0 }, 0, 1 };
    VkGraphicsPipelineCreateInfo gci = { 28, 0, 0, 2, st, &vis, &ias, 0, &vps, &rs, &ms, &z2, &cbs, &dys, tb_vk.pl, tb_vk.rp, 0, 0, -1 };
    if (tbvk_CreateGraphicsPipelines(tb_vk.dev, 0, 1, &gci, 0, &tb_vk.pipe)) return 0;
    gci.pDepthStencilState = &z3;
    if (tbvk_CreateGraphicsPipelines(tb_vk.dev, 0, 1, &gci, 0, &tb_vk.pipe3)) return 0;
    // третий: сетки целиком на видеокарте (tb3.vert) — без памяти фигур, всё из сеток и записей
    VkShaderModuleCreateInfo smv3 = { 16, 0, 0, sizeof tb_spv_vert3, tb_spv_vert3 };
    VkShaderModule mv3;
    VkPipelineVertexInputStateCreateInfo vis0 = { 19, 0, 0, 0, 0, 0, 0 };
    if (feat[40] && !getenv("TB_GPU_NOMESH") && tbvk_CreateShaderModule(tb_vk.dev, &smv3, 0, &mv3) == 0) {
        st[0].module = mv3;
        gci.pVertexInputState = &vis0;
        tb_gpu_mesh = tbvk_CreateGraphicsPipelines(tb_vk.dev, 0, 1, &gci, 0, &tb_vk.pipem) == 0;
    }
    for (int s = 0; s < TB_GSLOTS; s++)
        if (!tb_vk_buffer(sizeof(tb_ginst) * TB_GI_CAP, 0x80, &tb_vk.ibuf[s], &tb_vk.imap[s])) return 0;   // VERTEX_BUFFER
    if (!tb_vk_readback()) return 0;
    VkCommandPoolCreateInfo cpi = { 39, 0, 2, tb_vk.qf };   // RESET_COMMAND_BUFFER
    if (tbvk_CreateCommandPool(tb_vk.dev, &cpi, 0, &tb_vk.pool)) return 0;
    VkCommandBufferAllocateInfo cai = { 40, 0, tb_vk.pool, 0, TB_GSLOTS };
    if (tbvk_AllocateCommandBuffers(tb_vk.dev, &cai, tb_vk.cb)) return 0;
    VkFenceCreateInfo fi = { 8, 0, 1 }, si = { 9, 0, 0 };   // SIGNALED: набор свободен с самого начала
    for (int s = 0; s < TB_GSLOTS; s++)
        if (tbvk_CreateFence(tb_vk.dev, &fi, 0, &tb_vk.fence[s]) || tbvk_CreateSemaphore(tb_vk.dev, &si, 0, &tb_vk.sem_get[s])
            || tbvk_CreateSemaphore(tb_vk.dev, &si, 0, &tb_vk.sem_done[s])) return 0;
    tbvk_WaitForFences(tb_vk.dev, 1, &tb_vk.fence[0], 1, ~0ULL);
    tbvk_ResetFences(tb_vk.dev, 1, &tb_vk.fence[0]);
    tb_gi = (tb_ginst *)tb_vk.imap[0];
    if (tb_gpu_mesh) strcat(what, ", 3D тоже на ней");
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
    if (tb_img_dirty) tb_vk_images();
    if (tb_mesh_gdirty && tb_gpu_mesh) tb_vk_meshes();
    int s = tb_gslot;
    VkCommandBuffer cb = tb_vk.cb[s];
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
    if (tb_gin && tb_vk.zfresh) {   // новая глубина (начало или новый размер окна): в рабочее состояние
        tb_vk.zfresh = 0;
        tb_gzclr = 1;
        VkImageMemoryBarrier b = { 45, 0, 0, 0x200 | 0x400, 0, 3, 0xFFFFFFFFu, 0xFFFFFFFFu, tb_vk.zimg, { 2, 0, 1, 0, 1 } };
        tbvk_CmdPipelineBarrier(cb, 1, 0x100 | 0x200, 0, 0, 0, 0, 0, 1, &b);
    }
    if (tb_gin) {
        VkRenderPassBeginInfo rb = { 43, 0, tb_vk.rp, tb_vk.fb, { { 0, 0 }, { (uint32_t)tb_gw, (uint32_t)tb_gh } }, 0, 0 };
        tbvk_CmdBeginRenderPass(cb, &rb, 0);
        if (tb_gzclr) {   // начало кадра: глубина 3D — «далеко» везде
            tb_gzclr = 0;
            VkClearAttachment za = { 2, 0, { { 1.0f, 0, 0, 0 } } };
            VkClearRect zr = { { { 0, 0 }, { (uint32_t)tb_gw, (uint32_t)tb_gh } }, 0, 1 };
            tbvk_CmdClearAttachments(cb, 1, &za, 1, &zr);
        }
        tbvk_CmdBindPipeline(cb, 0, tb_vk.pipe);
        tbvk_CmdBindDescriptorSets(cb, 0, tb_vk.pl, 0, 1, &tb_vk.ds, 0, 0);
        VkDeviceSize off = 0;
        tbvk_CmdBindVertexBuffers(cb, 0, 1, &tb_vk.ibuf[s], &off);
        float size[2] = { (float)tb_gw, (float)tb_gh };
        tbvk_CmdPushConstants(cb, tb_vk.pl, 1, 0, 8, size);
        VkViewport vp = { 0, 0, (float)tb_gw, (float)tb_gh, 0, 1 };
        VkRect2D sc = { { 0, 0 }, { (uint32_t)tb_gw, (uint32_t)tb_gh } };
        tbvk_CmdSetViewport(cb, 0, 1, &vp);
        tbvk_CmdSetScissor(cb, 0, 1, &sc);
        // подряд идущие фигуры одного вида — одним вызовом: 2D или 3D (свой конвейер);
        // m = 3 — сетка на видеокарте: треугольников × записей, свои постоянные
        int i = 0;
        while (i < tb_gin) {
            if (tb_gi[i].m == 3u) {
                tb_gpc pc;
                tb_mesh_gpc(&tb_gi[i], s, &pc);
                tbvk_CmdBindPipeline(cb, 0, tb_vk.pipem);
                tbvk_CmdPushConstants(cb, tb_vk.pl, 1, 0, sizeof pc, &pc);
                tbvk_CmdDraw(cb, 4, (uint32_t)(pc.ntri * tb_gi[i].y1), 0, 0);
                tbvk_CmdPushConstants(cb, tb_vk.pl, 1, 0, 8, size);
                i++;
                continue;
            }
            int is3 = (tb_gi[i].m & 0xC0000003u) == 2u, j = i + 1;
            while (j < tb_gin && tb_gi[j].m != 3u && ((tb_gi[j].m & 0xC0000003u) == 2u) == is3) j++;
            tbvk_CmdBindPipeline(cb, 0, is3 ? tb_vk.pipe3 : tb_vk.pipe);
            tbvk_CmdDraw(cb, 4, (uint32_t)(j - i), 0, (uint32_t)i);
            i = j;
        }
        tbvk_CmdEndRenderPass(cb);
    }
    uint32_t idx = 0;
    int shown = 0;
    if (present) {
        VkResult r = tb_vk.sc ? tbvk_AcquireNextImageKHR(tb_vk.dev, tb_vk.sc, ~0ULL, tb_vk.sem_get[s], 0, &idx) : -1000001004;
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
    VkSubmitInfo si = { 4, 0, shown, &tb_vk.sem_get[s], &wst, 1, &cb, shown, &tb_vk.sem_done[s] };
    tbvk_QueueSubmit(tb_vk.q, 1, &si, tb_vk.fence[s]);
    if (shown) {
        VkPresentInfoKHR pi = { 1000001001, 0, 1, &tb_vk.sem_done[s], 1, &tb_vk.sc, &idx, 0 };
        VkResult r = tbvk_QueuePresentKHR(tb_vk.q, &pi);
        if (r == -1000001004 || r == 1000001003) tb_vk.sc_w = 0;   // пересоздать в следующий раз
    }
    // TB_GPU_CHECK: дождаться этого кадра, чтобы сверить; иначе видеокарта рисует, а мы идём дальше
    if (present && tb_gpu_back) {
        tbvk_WaitForFences(tb_vk.dev, 1, &tb_vk.fence[s], 1, ~0ULL);
        tb_gpu_compare();
    }
    if (shown && tb_vk.sc_w == 0) { tbvk_DeviceWaitIdle(tb_vk.dev); tb_vk_swapchain(); }
    // следующий набор: ждём, только если видеокарта ещё рисует из него кадр трёхкадровой давности
    tb_gslot = (s + 1) % TB_GSLOTS;
    tbvk_WaitForFences(tb_vk.dev, 1, &tb_vk.fence[tb_gslot], 1, ~0ULL);
    tbvk_ResetFences(tb_vk.dev, 1, &tb_vk.fence[tb_gslot]);
    tb_gi = (tb_ginst *)tb_vk.imap[tb_gslot];
    tb_gin = 0;
    tb_grn = 0;
    tb_gbatch++;
}
TB_API void tb_gpu_vsync(void) { tbvk_DeviceWaitIdle(tb_vk.dev); tb_vk_swapchain(); }
// окно стало размером tb_gw × tb_gh (было ow × oh): новый кадр, старое остаётся в левом верхнем углу
TB_API int tb_gpu_resize(int ow, int oh) {
    tbvk_DeviceWaitIdle(tb_vk.dev);
    VkImage oi = tb_vk.img; VkImageView ov = tb_vk.view; VkFramebuffer of = tb_vk.fb; VkDeviceMemory om = tb_vk.imem;
    VkImage ozi = tb_vk.zimg; VkImageView ozv = tb_vk.zview; VkDeviceMemory ozm = tb_vk.zmem;
    if (!tb_vk_target() || !tb_vk_readback()) return 0;
    if (tb_gpu_first) { ow = oh = 0; }   // старый кадр ещё пустой: копировать нечего
    VkCommandBuffer cb = tb_vk.cb[tb_gslot];
    VkCommandBufferBeginInfo bi = { 42, 0, 1, 0 };
    tbvk_ResetCommandBuffer(cb, 0);
    tbvk_BeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b[2] = { tb_vk_bar(tb_vk.img, 0, 0x1000, 0, 7), tb_vk_bar(oi, 0x100, 0x800, 2, 6) };
    tbvk_CmdPipelineBarrier(cb, 0x400 | 0x1000, 0x1000, 0, 0, 0, 0, 0, ow && oh ? 2 : 1, b);
    tb_gpu_first = 0;
    VkClearColorValue zero = { { 0, 0, 0, 0 } };
    VkImageSubresourceRange rg = { 1, 0, 1, 0, 1 };
    tbvk_CmdClearColorImage(cb, tb_vk.img, 7, &zero, 1, &rg);
    b[0] = tb_vk_bar(tb_vk.img, 0x1000, 0x1000, 7, 7);
    tbvk_CmdPipelineBarrier(cb, 0x1000, 0x1000, 0, 0, 0, 0, 0, 1, b);
    VkImageCopy cp = { { 1, 0, 0, 1 }, { 0, 0, 0 }, { 1, 0, 0, 1 }, { 0, 0, 0 },
        { (uint32_t)(ow < tb_gw ? ow : tb_gw), (uint32_t)(oh < tb_gh ? oh : tb_gh), 1 } };
    if (ow && oh) tbvk_CmdCopyImage(cb, oi, 6, tb_vk.img, 7, 1, &cp);
    b[0] = tb_vk_bar(tb_vk.img, 0x1000, 0x80 | 0x100, 7, 2);
    tbvk_CmdPipelineBarrier(cb, 0x1000, 0x400, 0, 0, 0, 0, 0, 1, b);
    tbvk_EndCommandBuffer(cb);
    VkSubmitInfo si = { 4, 0, 0, 0, 0, 1, &cb, 0, 0 };
    tbvk_QueueSubmit(tb_vk.q, 1, &si, 0);
    tbvk_DeviceWaitIdle(tb_vk.dev);
    tbvk_DestroyFramebuffer(tb_vk.dev, of, 0);
    tbvk_DestroyImageView(tb_vk.dev, ov, 0);
    tbvk_DestroyImage(tb_vk.dev, oi, 0);
    tbvk_FreeMemory(tb_vk.dev, om, 0);
    tbvk_DestroyImageView(tb_vk.dev, ozv, 0);
    tbvk_DestroyImage(tb_vk.dev, ozi, 0);
    tbvk_FreeMemory(tb_vk.dev, ozm, 0);
    tb_vk_swapchain();
    return 1;
}
#endif
#if defined(TB_BARE)
//@include omnium/kernel/screen.h
#elif defined(__APPLE__)
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
    "struct TbV { float4 pos [[position]]; int2 o [[flat]]; uint2 cm [[flat]]; int4 q [[flat]]; uint4 p [[flat]]; };   // p — плоскость глубины (tb_vert3)\n"
    "vertex TbV tb_vert(uint vid [[vertex_id]], uint iid [[instance_id]],\n"
    "                   const device TbInst *ins [[buffer(0)]], constant float2 &size [[buffer(1)]]) {\n"
    "    TbInst t = ins[iid];\n"
    "    int4 b = t.q;\n"
    "    if (t.cm.y != 0u && (t.cm.y >> 30) == 0u) {   // треугольник (2D и 3D): квадрат вокруг трёх углов, внутри решает tb_frag\n"
    "        b.xy = min(min(t.q.xy, t.q.zw), t.o);\n"
    "        b.zw = max(max(t.q.xy, t.q.zw), t.o) + 1;\n"
    "        b = clamp(b, int4(0), int4(int(size.x), int(size.y), int(size.x), int(size.y)));\n"
    "    }\n"
    "    float2 p = float2((vid & 1) ? b.z : b.x, (vid & 2) ? b.w : b.y);\n"
    "    TbV out;\n"
    "    out.pos = float4(p.x / size.x * 2.0 - 1.0, 1.0 - p.y / size.y * 2.0, 0.0, 1.0);\n"
    "    out.o = t.o;\n"
    "    out.cm = t.cm;\n"
    "    out.q = t.q;\n"
    "    out.p = uint4(0u);\n"
    "    return out;\n"
    "}\n"
    "// сторона a→b треугольника: то же правило, что tb_tri_edge в runtime.h\n"
    "static bool tb_edge(int2 a, int2 b, int2 p) {\n"
    "    int ex = b.x - a.x, ey = b.y - a.y, e = ex * (2 * p.y + 1 - 2 * a.y) - ey * (2 * p.x + 1 - 2 * a.x);\n"
    "    return e > 0 || (e == 0 && (ey > 0 || (ey == 0 && ex < 0)));\n"
    "}\n"
    "struct TbOut { float4 c [[color(0)]]; float d [[depth(any)]]; };\n"
    "fragment TbOut tb_frag(TbV v [[stage_in]], constant uint4 *font [[buffer(0)]], const device uint *img [[buffer(1)]],\n"
    "                       const device uint4 *pl [[buffer(2)]]) {\n"
    "    int2 p = int2(v.pos.xy);\n"
    "    uint m = v.cm.y, k = m >> 30;\n"
    "    float depth = 0.0;\n"
    "    if (m != 0u && k == 0u) {   // треугольник: m = 1 — плоский, m = 2 | (место << 2) — из сетки (3D), m = 3 — сетка с видеокарты (tb_vert3)\n"
    "        if (!tb_edge(v.q.xy, v.q.zw, p) || !tb_edge(v.q.zw, v.o, p) || !tb_edge(v.o, v.q.xy, p)) discard_fragment();\n"
    "        if ((m & 2u) != 0u) {   // глубина: та же целая формула, что tb_tri3_depth в runtime.h\n"
    "            uint4 P = m == 3u ? v.p : pl[m >> 2];\n"
    "            uint kk = P.z + P.x * uint(2 * p.x + 1) + P.y * uint(2 * p.y + 1);\n"
    "            depth = float(clamp(as_type<int>(kk) >> 6, 0, 16777215)) / 16777216.0;\n"
    "        }\n"
    "    } else if (k == 1u) {\n"
    "        int r = int(m & 0x3FFFFFu), i = p.x - v.o.x, j = p.y - v.o.y;\n"
    "        if (i * i + j * j > r * r + r) discard_fragment();\n"
    "    } else if (k == 2u) {\n"
    "        int s = int(m & 0x3FFFFFu), cx = (p.x - v.o.x) / s, ry = (p.y - v.o.y) / s;\n"
    "        uint g = (m >> 22) & 255u;\n"
    "        uint b = (font[g][ry >> 2] >> (uint(ry & 3) * 8u)) & 255u;\n"
    "        if ((b & (0x80u >> uint(cx))) == 0u) discard_fragment();\n"
    "    }\n"
    "    uint c = v.cm.x, a = 255u;\n"
    "    if (k == 3u) {\n"
    "        uint px = img[v.cm.x + uint(p.y - v.o.y) * (m & 0x3FFFFFu) + uint(p.x - v.o.x)];\n"
    "        a = px >> 24;\n"
    "        if ((m & (1u << 29)) != 0u) { if (a == 0u) discard_fragment(); }   // буква шрифта: смешать с фоном\n"
    "        else { if (a < 128u) discard_fragment(); a = 255u; }\n"
    "        c = px;\n"
    "    }\n"
    "    TbOut o;\n"
    "    // смешивание One, OneMinusSourceAlpha: своя часть цвета (c * a / 255) — целыми числами здесь, как tb_draw_image_a\n"
    "    uint3 kk = (uint3((c >> 16) & 255u, (c >> 8) & 255u, c & 255u) * a + 127u) / 255u;\n"
    "    o.c = float4(float3(kk), float(a)) / 255.0;\n"
    "    o.d = depth;\n"
    "    return o;\n"
    "}\n";
TB_API const char tb_msl3[] =
    "// 3D на видеокарте (Mac): то же, что tb3.vert — целые числа, та же формула, что tb_m3_* в runtime.h.\n"
    "// Собирается отдельно, вместе с tb.metal: не собралось — сетки считает процессор, как раньше\n"
    "struct TbL3 { long x, y, z; };   // три целых по 64 бита\n"
    "struct TbPC { float2 size; int tbase, ntri, rbase, f, gw, gh, cull; };\n"
    "static long tb_fdiv(long a, long b) {   // деление с округлением вниз (b > 0)\n"
    "    if (a >= 0) return long(ulong(a) / ulong(b));\n"
    "    return -long((ulong(-a) + ulong(b) - 1ul) / ulong(b));\n"
    "}\n"
    "static TbL3 tb_corner(int4 p, const device int4 *rec, int ri, constant TbPC &pc) {\n"
    "    long c[3];\n"
    "    for (int k = 0; k < 3; k++) {\n"
    "        int4 m = rec[ri + k];\n"
    "        c[k] = ((long(m.x) * long(p.x) + long(m.y) * long(p.y) + long(m.z) * long(p.z)) >> 30) + long(m.w);\n"
    "    }\n"
    "    long f = long(pc.f);\n"
    "    TbL3 o;\n"
    "    o.x = (long(pc.gw) * 128 + tb_fdiv(f * c[0], c[2]) + 128) >> 8;\n"
    "    o.y = (long(pc.gh) * 128 - tb_fdiv(f * c[1], c[2]) + 128) >> 8;\n"
    "    o.z = 16777216 - tb_fdiv(long(205) << 24, c[2]);\n"
    "    return o;\n"
    "}\n"
    "vertex TbV tb_vert3(uint vid [[vertex_id]], uint iid [[instance_id]], constant TbPC &pc [[buffer(1)]],\n"
    "                    const device int4 *mesh [[buffer(3)]], const device int4 *rec [[buffer(4)]]) {\n"
    "    int ii = int(iid), d = ii / pc.ntri, t = ii - d * pc.ntri;\n"
    "    int4 tr = mesh[pc.tbase + t];\n"
    "    int ri = pc.rbase + d * 3;\n"
    "    TbL3 a = tb_corner(mesh[tr.x], rec, ri, pc), b = tb_corner(mesh[tr.y], rec, ri, pc), c = tb_corner(mesh[tr.z], rec, ri, pc);\n"
    "    long area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);\n"
    "    bool skip = area == 0 || (pc.cull > 0 && area > 0) || (pc.cull < 0 && area < 0);\n"
    "    if (area < 0) { TbL3 k = b; b = c; c = k; area = -area; }\n"
    "    if (skip) area = 1;\n"
    "    long e1x = b.x - a.x, e1y = b.y - a.y, e2x = c.x - a.x, e2y = c.y - a.y, d1 = b.z - a.z, d2 = c.z - a.z;\n"
    "    long A = tb_fdiv(64 * (d1 * e2y - d2 * e1y) + area, 2 * area), B = tb_fdiv(64 * (d2 * e1x - d1 * e2x) + area, 2 * area);\n"
    "    long C = 64 * a.z - 2 * A * a.x - 2 * B * a.y;\n"
    "    int4 q = int4(int(a.x), int(a.y), int(b.x), int(b.y));\n"
    "    int2 o = int2(int(c.x), int(c.y));\n"
    "    int4 bb;\n"
    "    bb.xy = min(min(q.xy, q.zw), o);\n"
    "    bb.zw = max(max(q.xy, q.zw), o) + 1;\n"
    "    bb = clamp(bb, int4(0), int4(int(pc.size.x), int(pc.size.y), int(pc.size.x), int(pc.size.y)));\n"
    "    if (skip) bb = int4(0);\n"
    "    float2 p = float2((vid & 1) ? bb.z : bb.x, (vid & 2) ? bb.w : bb.y);\n"
    "    TbV out;\n"
    "    out.pos = float4(p.x / pc.size.x * 2.0 - 1.0, 1.0 - p.y / pc.size.y * 2.0, 0.0, 1.0);\n"
    "    out.o = o;\n"
    "    out.cm = uint2(uint(tr.w) & 0xFFFFFFu, 3u);\n"
    "    out.q = q;\n"
    "    out.p = uint4(uint(ulong(A) & 0xFFFFFFFFul), uint(ulong(B) & 0xFFFFFFFFul), uint(ulong(C) & 0xFFFFFFFFul), 0u);\n"
    "    return out;\n"
    "}\n";
// @@MSL-END@@
TB_API const unsigned char tb_mac_keys[128] = {
    [0]='A',[1]='S',[2]='D',[3]='F',[4]='H',[5]='G',[6]='Z',[7]='X',[8]='C',[9]='V',[11]='B',[12]='Q',[13]='W',
    [14]='E',[15]='R',[16]='Y',[17]='T',[18]='1',[19]='2',[20]='3',[21]='4',[22]='6',[23]='5',[25]='9',[26]='7',
    [28]='8',[29]='0',[31]='O',[32]='U',[34]='I',[35]='P',[36]=TB_K_ENTER,[37]='L',[38]='J',[40]='K',[45]='N',
    [46]='M',[48]=TB_K_TAB,[49]=TB_K_SPACE,[51]=TB_K_BACK,[53]=TB_K_ESC,[56]=TB_K_SHIFT,[60]=TB_K_SHIFT,
    [76]=TB_K_ENTER,[123]=TB_K_LEFT,[124]=TB_K_RIGHT,[125]=TB_K_DOWN,[126]=TB_K_UP,
    [27]='-',[24]='=',[33]='[',[30]=']',[41]=';',[39]='\'',[43]=',',[47]='.',[44]='/',[50]='`',[42]='\\',
    [117]=TB_K_DEL,[115]=TB_K_HOME,[119]=TB_K_END,[116]=TB_K_PGUP,[121]=TB_K_PGDN,
    [122]=TB_K_F1,[120]=TB_K_F1+1,[99]=TB_K_F1+2,[118]=TB_K_F1+3,[96]=TB_K_F1+4,[97]=TB_K_F1+5,
    [98]=TB_K_F1+6,[100]=TB_K_F1+7,[101]=TB_K_F1+8,[109]=TB_K_F1+9,[103]=TB_K_F1+10,[111]=TB_K_F1+11 };
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
TB_API double tb_gfx_scale(void) {
    id scr = TB_MSG(id, TB_CLS("NSScreen"), "mainScreen");
    return scr ? TB_MSG(double, scr, "backingScaleFactor") : 1;
}
// буфер обмена Mac: NSPasteboard, текст UTF-8
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *);
TB_API id tb_ns_str(const char *s) { return TB_MSG1(id, TB_CLS("NSString"), "stringWithUTF8String:", const char *, s); }
TB_API char *tb_gfx_clip_get(void) {
    void *pool = objc_autoreleasePoolPush();
    id pb = TB_MSG(id, TB_CLS("NSPasteboard"), "generalPasteboard");
    id str = pb ? TB_MSG1(id, pb, "stringForType:", id, tb_ns_str("public.utf8-plain-text")) : 0;
    const char *u = str ? TB_MSG(const char *, str, "UTF8String") : 0;
    char *out = u ? strdup(u) : 0;
    objc_autoreleasePoolPop(pool);
    return out;
}
TB_API void tb_gfx_clip_set(const char *s) {
    void *pool = objc_autoreleasePoolPush();
    id pb = TB_MSG(id, TB_CLS("NSPasteboard"), "generalPasteboard");
    if (pb) {
        TB_MSG(long, pb, "clearContents");
        ((BOOL (*)(id, SEL, id, id))objc_msgSend)(pb, sel_getUid("setString:forType:"), tb_ns_str(s), tb_ns_str("public.utf8-plain-text"));
    }
    objc_autoreleasePoolPop(pool);
}
// курсор Mac: ставится заново каждый кадр (AppKit сам возвращает стрелку, когда мышь входит в окно)
TB_API int tb_mac_cur = 0, tb_mac_hidden = 0;
TB_API void tb_mac_cursor_apply(void) {
    static const char *names[7] = { "arrowCursor", "pointingHandCursor", "IBeamCursor", "openHandCursor", "resizeLeftRightCursor", "resizeUpDownCursor", "arrowCursor" };
    if (tb_mac_cur == 7) { if (!tb_mac_hidden) { TB_MSG(void, TB_CLS("NSCursor"), "hide"); tb_mac_hidden = 1; } return; }
    if (tb_mac_hidden) { TB_MSG(void, TB_CLS("NSCursor"), "unhide"); tb_mac_hidden = 0; }
    if (tb_mac_cur == 0) return;   // обычная стрелка — её ставит сам Mac
    id c = TB_MSG(id, TB_CLS("NSCursor"), names[tb_mac_cur]);
    if (c) TB_MSG(void, c, "set");
}
TB_API void tb_gfx_cursor(int k) {
    int was = tb_mac_cur;
    tb_mac_cur = k;
    if (k == 0 && was != 0 && was != 7) TB_MSG(void, TB_MSG(id, TB_CLS("NSCursor"), "arrowCursor"), "set");
    tb_mac_cursor_apply();
}
// ---------- Metal (Mac) ----------
// Metal зовём так же, как Cocoa: через objc_msgSend. Шейдер (tb_msl выше, compiler/gpu/tb.metal)
// Mac переводит сам при запуске. Сама библиотека Metal грузится через dlopen: нет её — процессор.
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *);
typedef struct { unsigned long x, y, z; } tb_mtl3;          // MTLOrigin и MTLSize
typedef struct { double r, g, b, a; } tb_mtl_color;         // MTLClearColor
TB_API struct { id dev, queue, pso, tex, ibuf[TB_GSLOTS], cbs[TB_GSLOTS], fbuf, rbuf, pbuf, layer, ztex, zs2, zs3, gpbuf,
    psom, mbuf, grbuf; } tb_mtl;   // psom — сетки на видеокарте (tb_vert3), mbuf — сетки, grbuf — записи draw_mesh
// дождаться всех кадров, которые видеокарта ещё рисует
TB_API void tb_mtl_idle(void) {
    for (int s = 0; s < TB_GSLOTS; s++)
        if (tb_mtl.cbs[s]) { TB_MSG(void, tb_mtl.cbs[s], "waitUntilCompleted"); TB_MSG(void, tb_mtl.cbs[s], "release"); tb_mtl.cbs[s] = 0; }
}
// картинки поменялись (load_image): все точки — в новый буфер видеокарты
TB_API int tb_mtl_images(void) {
    tb_mtl_idle();
    if (tb_mtl.pbuf) TB_MSG(void, tb_mtl.pbuf, "release");
    unsigned long sz = tb_imgpx_n * 4 > 16 ? tb_imgpx_n * 4 : 16;
    tb_mtl.pbuf = ((id (*)(id, SEL, unsigned long, unsigned long))objc_msgSend)(tb_mtl.dev, sel_getUid("newBufferWithLength:options:"), sz, 0);
    if (!tb_mtl.pbuf) return 0;
    if (tb_imgpx_n) memcpy(TB_MSG(void *, tb_mtl.pbuf, "contents"), tb_imgpx, tb_imgpx_n * 4);
    tb_img_dirty = 0;
    return 1;
}
// сетки поменялись (mesh_add): все сетки — в новый буфер видеокарты
TB_API int tb_mtl_meshes(void) {
    tb_mtl_idle();
    if (tb_mtl.mbuf) TB_MSG(void, tb_mtl.mbuf, "release");
    long long n = tb_mesh_pack(0);
    unsigned long sz = n > 0 ? (unsigned long)n * 16 : 16;
    tb_mtl.mbuf = ((id (*)(id, SEL, unsigned long, unsigned long))objc_msgSend)(tb_mtl.dev, sel_getUid("newBufferWithLength:options:"), sz, 0);
    if (!tb_mtl.mbuf) return 0;
    if (n > 0) tb_mesh_pack((int32_t *)TB_MSG(void *, tb_mtl.mbuf, "contents"));
    tb_mesh_gdirty = 0;
    return 1;
}
TB_API void tb_gpu_vsync(void) { TB_MSG1(void, tb_mtl.layer, "setDisplaySyncEnabled:", BOOL, tb_gpu_nosync ? NO : YES); }
// свой кадр на видеокарте размером tb_gw × tb_gh
TB_API id tb_mtl_texture(void) {
    id td = ((id (*)(id, SEL, unsigned long, unsigned long, unsigned long, BOOL))objc_msgSend)(TB_CLS("MTLTextureDescriptor"),
        sel_getUid("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"), 80, (unsigned long)tb_gw, (unsigned long)tb_gh, NO);
    TB_MSG1(void, td, "setUsage:", unsigned long, 1 | 4);      // ShaderRead | RenderTarget
    TB_MSG1(void, td, "setStorageMode:", unsigned long, 2);    // Private: только на видеокарте
    return TB_MSG1(id, tb_mtl.dev, "newTextureWithDescriptor:", id, td);
}
// глубина для 3D (Depth32Float), только на видеокарте
TB_API id tb_mtl_ztex(void) {
    id td = ((id (*)(id, SEL, unsigned long, unsigned long, unsigned long, BOOL))objc_msgSend)(TB_CLS("MTLTextureDescriptor"),
        sel_getUid("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"), 252, (unsigned long)tb_gw, (unsigned long)tb_gh, NO);
    TB_MSG1(void, td, "setUsage:", unsigned long, 4);          // RenderTarget
    TB_MSG1(void, td, "setStorageMode:", unsigned long, 2);    // Private
    return TB_MSG1(id, tb_mtl.dev, "newTextureWithDescriptor:", id, td);
}
// правило глубины: 2D — не трогает (Always, без записи); 3D — ближе рисуется (Less) и пишет глубину
TB_API id tb_mtl_zstate(unsigned long cmp, BOOL write) {
    id d = TB_MSG(id, TB_MSG(id, TB_CLS("MTLDepthStencilDescriptor"), "alloc"), "init");
    TB_MSG1(void, d, "setDepthCompareFunction:", unsigned long, cmp);
    TB_MSG1(void, d, "setDepthWriteEnabled:", BOOL, write);
    id st = TB_MSG1(id, tb_mtl.dev, "newDepthStencilStateWithDescriptor:", id, d);
    TB_MSG(void, d, "release");
    return st;
}
TB_API int tb_mtl_readback(void) {
    if (!tb_gpu_check) return 1;
    if (tb_mtl.rbuf) TB_MSG(void, tb_mtl.rbuf, "release");
    tb_mtl.rbuf = ((id (*)(id, SEL, unsigned long, unsigned long))objc_msgSend)(tb_mtl.dev, sel_getUid("newBufferWithLength:options:"),
        (unsigned long)tb_gw * tb_gh * 4, 0);
    if (!tb_mtl.rbuf) return 0;
    tb_gpu_back = (uint32_t *)TB_MSG(void *, tb_mtl.rbuf, "contents");
    return 1;
}
// окно стало размером tb_gw × tb_gh (было ow × oh): новый кадр, старое остаётся в левом верхнем углу
TB_API int tb_gpu_resize(int ow, int oh) {
    tb_mtl_idle();
    if (tb_gpu_first) ow = oh = 0;   // старый кадр ещё пустой: копировать нечего
    tb_gpu_first = 0;
    id old = tb_mtl.tex;
    tb_mtl.tex = tb_mtl_texture();
    if (!tb_mtl.tex || !tb_mtl_readback()) return 0;
    if (tb_mtl.ztex) TB_MSG(void, tb_mtl.ztex, "release");
    tb_mtl.ztex = tb_mtl_ztex();
    if (!tb_mtl.ztex) return 0;
    tb_gzclr = 1;
    void *pool = objc_autoreleasePoolPush();
    id cb = TB_MSG(id, tb_mtl.queue, "commandBuffer");
    id rpd = TB_MSG(id, TB_CLS("MTLRenderPassDescriptor"), "renderPassDescriptor");
    id ca = TB_MSG1(id, TB_MSG(id, rpd, "colorAttachments"), "objectAtIndexedSubscript:", unsigned long, 0);
    TB_MSG1(void, ca, "setTexture:", id, tb_mtl.tex);
    TB_MSG1(void, ca, "setLoadAction:", unsigned long, 2);
    tb_mtl_color z = { 0, 0, 0, 0 };
    TB_MSG1(void, ca, "setClearColor:", tb_mtl_color, z);
    TB_MSG1(void, ca, "setStoreAction:", unsigned long, 1);
    TB_MSG(void, TB_MSG1(id, cb, "renderCommandEncoderWithDescriptor:", id, rpd), "endEncoding");
    if (ow && oh) {
        id bl = TB_MSG(id, cb, "blitCommandEncoder");
        tb_mtl3 o = { 0, 0, 0 }, sz = { (unsigned long)(ow < tb_gw ? ow : tb_gw), (unsigned long)(oh < tb_gh ? oh : tb_gh), 1 };
        ((void (*)(id, SEL, id, unsigned long, unsigned long, tb_mtl3, tb_mtl3, id, unsigned long, unsigned long, tb_mtl3))objc_msgSend)(bl,
            sel_getUid("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:"),
            old, 0, 0, o, sz, tb_mtl.tex, 0, 0, o);
        TB_MSG(void, bl, "endEncoding");
    }
    TB_MSG(void, cb, "commit");
    TB_MSG(void, cb, "waitUntilCompleted");
    objc_autoreleasePoolPop(pool);
    TB_MSG(void, old, "release");
    TB_MSG1(void, tb_mtl.layer, "setDrawableSize:", CGSize, CGSizeMake(tb_gw, tb_gh));
    return 1;
}
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
    TB_MSG1(void, ca, "setBlendingEnabled:", BOOL, YES);   // буквы шрифта: One, OneMinusSourceAlpha (своя часть цвета — в шейдере)
    TB_MSG1(void, ca, "setSourceRGBBlendFactor:", unsigned long, 1);
    TB_MSG1(void, ca, "setDestinationRGBBlendFactor:", unsigned long, 5);
    TB_MSG1(void, pd, "setDepthAttachmentPixelFormat:", unsigned long, 252);   // Depth32Float: глубина 3D
    tb_mtl.pso = ((id (*)(id, SEL, id, id *))objc_msgSend)(dev, sel_getUid("newRenderPipelineStateWithDescriptor:error:"), pd, &err);
    if (!tb_mtl.pso) return tb_mtl_fail("не собрался конвейер:", err);
    // сетки целиком на видеокарте (tb3.metal, 64-битные целые): свой шейдер; не собрался — сетки на процессоре
    if (!getenv("TB_GPU_NOMESH")) {
        size_t n1 = strlen(tb_msl), n2 = strlen(tb_msl3);
        char *src = (char *)malloc(n1 + n2 + 1);
        memcpy(src, tb_msl, n1); memcpy(src + n1, tb_msl3, n2 + 1);
        id err3 = 0;
        id sh3 = ((id (*)(id, SEL, id, id, id *))objc_msgSend)(dev, sel_getUid("newLibraryWithSource:options:error:"), tb_nsstr(src), 0, &err3);
        free(src);
        id vf3 = sh3 ? TB_MSG1(id, sh3, "newFunctionWithName:", id, tb_nsstr("tb_vert3")) : 0;
        id ff3 = sh3 ? TB_MSG1(id, sh3, "newFunctionWithName:", id, tb_nsstr("tb_frag")) : 0;
        if (vf3 && ff3) {
            TB_MSG1(void, pd, "setVertexFunction:", id, vf3);
            TB_MSG1(void, pd, "setFragmentFunction:", id, ff3);
            tb_mtl.psom = ((id (*)(id, SEL, id, id *))objc_msgSend)(dev, sel_getUid("newRenderPipelineStateWithDescriptor:error:"), pd, &err3);
        }
        if (!tb_mtl.psom) tb_mtl_fail("3D на видеокарте не собралось (сетки считает процессор):", err3);
    }
    // свой кадр на видеокарте: живёт между кадрами, как tb_px
    tb_mtl.dev = dev;
    tb_mtl.tex = tb_mtl_texture();
    tb_mtl.ztex = tb_mtl_ztex();
    tb_mtl.zs2 = tb_mtl_zstate(7, NO);    // Always
    tb_mtl.zs3 = tb_mtl_zstate(1, YES);   // Less
    tb_mtl.gpbuf = ((id (*)(id, SEL, unsigned long, unsigned long))objc_msgSend)(dev, sel_getUid("newBufferWithLength:options:"),
        sizeof(tb_gplane_t) * TB_GI_CAP * TB_GSLOTS, 0);
    if (!tb_mtl.ztex || !tb_mtl.zs2 || !tb_mtl.zs3 || !tb_mtl.gpbuf) return tb_mtl_fail("нет глубины для 3D", 0);
    tb_gplane = (tb_gplane_t *)TB_MSG(void *, tb_mtl.gpbuf, "contents");
    tb_mtl.grbuf = ((id (*)(id, SEL, unsigned long, unsigned long))objc_msgSend)(dev, sel_getUid("newBufferWithLength:options:"),
        sizeof(tb_grec) * TB_GR_CAP * TB_GSLOTS, 0);
    if (!tb_mtl.grbuf || !tb_mtl_meshes()) return tb_mtl_fail("нет памяти на видеокарте", 0);
    tb_grec_p = (tb_grec *)TB_MSG(void *, tb_mtl.grbuf, "contents");
    tb_gpu_mesh = tb_mtl.psom != 0;
    for (int s = 0; s < TB_GSLOTS; s++) {
        tb_mtl.ibuf[s] = ((id (*)(id, SEL, unsigned long, unsigned long))objc_msgSend)(dev, sel_getUid("newBufferWithLength:options:"),
            sizeof(tb_ginst) * TB_GI_CAP, 0);
        if (!tb_mtl.ibuf[s]) return tb_mtl_fail("нет памяти на видеокарте", 0);
    }
    tb_mtl.fbuf = ((id (*)(id, SEL, const void *, unsigned long, unsigned long))objc_msgSend)(dev,
        sel_getUid("newBufferWithBytes:length:options:"), tb_font, sizeof tb_font, 0);
    if (!tb_mtl.tex || !tb_mtl.fbuf) return tb_mtl_fail("нет памяти на видеокарте", 0);
    if (!tb_mtl_readback()) return tb_mtl_fail("нет памяти на видеокарте", 0);
    tb_mtl.queue = TB_MSG(id, dev, "newCommandQueue");
    // слой окна для Metal: кадр того же размера, Retina растягивает его без сглаживания
    id layer = TB_MSG(id, TB_CLS("CAMetalLayer"), "layer");
    TB_MSG1(void, layer, "setDevice:", id, dev);
    TB_MSG1(void, layer, "setPixelFormat:", unsigned long, 80);
    TB_MSG1(void, layer, "setFramebufferOnly:", BOOL, NO);     // в кадр окна копируем (blit)
    TB_MSG1(void, layer, "setDrawableSize:", CGSize, CGSizeMake(tb_gw, tb_gh));
    TB_MSG1(void, layer, "setMagnificationFilter:", id, tb_nsstr("nearest"));
    TB_MSG1(void, layer, "setContentsScale:", double, tb_sc);   // window_sharp: точка кадра = точка экрана
    TB_MSG1(void, layer, "setColorspace:", CGColorSpaceRef, tb_cgspace);
    // кадр показывается вместе с обновлением экрана (без разрывов картинки).
    // TB_FPS_FREE=1 (замеры скорости): не ждать экран
    if (tb_gpu_nosync) TB_MSG1(void, layer, "setDisplaySyncEnabled:", BOOL, NO);
    TB_MSG1(void, view, "setLayer:", id, layer);
    TB_MSG1(void, view, "setWantsLayer:", BOOL, YES);
    tb_mtl.layer = layer;
    tb_mtl.dev = dev;
    if (!tb_mtl_images()) return tb_mtl_fail("нет памяти на видеокарте", 0);
    tb_gi = (tb_ginst *)TB_MSG(void *, tb_mtl.ibuf[0], "contents");
    char what[300];
    const char *name = TB_MSG(const char *, TB_MSG(id, dev, "name"), "UTF8String");
    snprintf(what, sizeof what, "видеокарта (Metal, %s%s)", name ? name : "?", tb_gpu_mesh ? ", 3D тоже на ней" : "");
    tb_gpu_say(what);
    return 1;
}
// нарисовать накопленные фигуры; present — ещё и показать кадр в окне
TB_API void tb_gpu_flush(int present) {
    if (!tb_gin && !present && !tb_gpu_first) return;
    if (tb_img_dirty) tb_mtl_images();
    if (tb_mesh_gdirty && tb_gpu_mesh) tb_mtl_meshes();
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
        // глубина 3D: в начале кадра стирается («далеко»), дальше сохраняется между проходами
        id za = TB_MSG(id, rpd, "depthAttachment");
        TB_MSG1(void, za, "setTexture:", id, tb_mtl.ztex);
        TB_MSG1(void, za, "setLoadAction:", unsigned long, tb_gzclr ? 2 : 1);
        TB_MSG1(void, za, "setClearDepth:", double, 1.0);
        TB_MSG1(void, za, "setStoreAction:", unsigned long, 1);
        tb_gzclr = 0;
        id enc = TB_MSG1(id, cb, "renderCommandEncoderWithDescriptor:", id, rpd);
        if (tb_gin) {
            float size[2] = { (float)tb_gw, (float)tb_gh };
            TB_MSG1(void, enc, "setRenderPipelineState:", id, tb_mtl.pso);
            ((void (*)(id, SEL, id, unsigned long, unsigned long))objc_msgSend)(enc, sel_getUid("setVertexBuffer:offset:atIndex:"), tb_mtl.ibuf[tb_gslot], 0, 0);
            ((void (*)(id, SEL, const void *, unsigned long, unsigned long))objc_msgSend)(enc, sel_getUid("setVertexBytes:length:atIndex:"), size, 8, 1);
            ((void (*)(id, SEL, id, unsigned long, unsigned long))objc_msgSend)(enc, sel_getUid("setFragmentBuffer:offset:atIndex:"), tb_mtl.fbuf, 0, 0);
            ((void (*)(id, SEL, id, unsigned long, unsigned long))objc_msgSend)(enc, sel_getUid("setFragmentBuffer:offset:atIndex:"), tb_mtl.pbuf, 0, 1);
            ((void (*)(id, SEL, id, unsigned long, unsigned long))objc_msgSend)(enc, sel_getUid("setFragmentBuffer:offset:atIndex:"), tb_mtl.gpbuf, 0, 2);
            if (tb_gpu_mesh) {
                ((void (*)(id, SEL, id, unsigned long, unsigned long))objc_msgSend)(enc, sel_getUid("setVertexBuffer:offset:atIndex:"), tb_mtl.mbuf, 0, 3);
                ((void (*)(id, SEL, id, unsigned long, unsigned long))objc_msgSend)(enc, sel_getUid("setVertexBuffer:offset:atIndex:"), tb_mtl.grbuf, 0, 4);
            }
            // подряд идущие фигуры одного вида — одним вызовом: 2D или 3D (своё правило глубины);
            // m = 3 — сетка на видеокарте: треугольников × записей, свой конвейер и постоянные
            int i = 0;
            while (i < tb_gin) {
                if (tb_gi[i].m == 3u) {
                    tb_gpc pc;
                    tb_mesh_gpc(&tb_gi[i], tb_gslot, &pc);
                    TB_MSG1(void, enc, "setRenderPipelineState:", id, tb_mtl.psom);
                    TB_MSG1(void, enc, "setDepthStencilState:", id, tb_mtl.zs3);
                    ((void (*)(id, SEL, const void *, unsigned long, unsigned long))objc_msgSend)(enc, sel_getUid("setVertexBytes:length:atIndex:"), &pc, sizeof pc, 1);
                    ((void (*)(id, SEL, unsigned long, unsigned long, unsigned long, unsigned long))objc_msgSend)(enc,
                        sel_getUid("drawPrimitives:vertexStart:vertexCount:instanceCount:"), 4, 0, 4, (unsigned long)pc.ntri * (unsigned long)tb_gi[i].y1);
                    TB_MSG1(void, enc, "setRenderPipelineState:", id, tb_mtl.pso);
                    ((void (*)(id, SEL, const void *, unsigned long, unsigned long))objc_msgSend)(enc, sel_getUid("setVertexBytes:length:atIndex:"), size, 8, 1);
                    i++;
                    continue;
                }
                int is3 = (tb_gi[i].m & 0xC0000003u) == 2u, j = i + 1;
                while (j < tb_gin && tb_gi[j].m != 3u && ((tb_gi[j].m & 0xC0000003u) == 2u) == is3) j++;
                TB_MSG1(void, enc, "setDepthStencilState:", id, is3 ? tb_mtl.zs3 : tb_mtl.zs2);
                ((void (*)(id, SEL, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long))objc_msgSend)(enc,
                    sel_getUid("drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:"), 4, 0, 4,
                    (unsigned long)(j - i), (unsigned long)i);   // TriangleStrip
                i = j;
            }
        }
        TB_MSG(void, enc, "endEncoding");
    }
    // без ожидания экрана (set_fps(0)): кадр рисуется всегда, а показывается не чаще 250 раз в секунду —
    // окно Mac не берёт кадры быстрее, и ожидание следующего кадра окна держало бы всю программу
    static long long last_show = 0;
    if (present && tb_gpu_nosync && !tb_gpu_back) {
        long long now = tb_time_us();
        if (now - last_show < 4000) present = 0; else last_show = now;
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
    // TB_GPU_CHECK: дождаться этого кадра, чтобы сверить; иначе видеокарта рисует, а мы идём дальше
    if (present && tb_gpu_back) TB_MSG(void, cb, "waitUntilCompleted");
    tb_mtl.cbs[tb_gslot] = TB_MSG(id, cb, "retain");
    objc_autoreleasePoolPop(pool);
    if (present && tb_gpu_back) tb_gpu_compare();
    // следующий набор: ждём, только если видеокарта ещё рисует из него кадр трёхкадровой давности
    tb_gslot = (tb_gslot + 1) % TB_GSLOTS;
    id old = tb_mtl.cbs[tb_gslot];
    if (old) { TB_MSG(void, old, "waitUntilCompleted"); TB_MSG(void, old, "release"); tb_mtl.cbs[tb_gslot] = 0; }
    tb_gi = (tb_ginst *)TB_MSG(void *, tb_mtl.ibuf[tb_gslot], "contents");
    tb_gin = 0;
    tb_grn = 0;
    tb_gbatch++;
}
TB_API int tb_gfx_open(const char *title) {
    tb_nsapp = TB_MSG(id, TB_CLS("NSApplication"), "sharedApplication");
    TB_MSG1(void, tb_nsapp, "setActivationPolicy:", long, 0);
    tb_wnd = ((id(*)(id, SEL, CGRect, unsigned long, unsigned long, BOOL))objc_msgSend)(
        TB_MSG(id, TB_CLS("NSWindow"), "alloc"), sel_getUid("initWithContentRect:styleMask:backing:defer:"),
        CGRectMake(0, 0, tb_gw / tb_sc, tb_gh / tb_sc), 1 | 2 | 4, 2, NO);
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
    tb_mac_cur = tb_cursor_want;
    if (!tb_gpu_on) {
        tb_gpu_say("процессор");
        TB_MSG1(void, view, "setWantsLayer:", BOOL, YES);
        tb_layer = TB_MSG(id, view, "layer");
        id nearest = TB_MSG1(id, TB_CLS("NSString"), "stringWithUTF8String:", const char *, "nearest");
        TB_MSG1(void, tb_layer, "setMagnificationFilter:", id, nearest);
        TB_MSG1(void, tb_layer, "setContentsScale:", double, tb_sc);
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
TB_API void tb_gfx_resizable(void) { TB_MSG1(void, tb_wnd, "setStyleMask:", unsigned long, 1 | 2 | 4 | 8); }
TB_API void tb_gfx_reimage(void) {
    if (tb_gpu_on) return;
    for (int i = 0; i < 3; i++) { free(tb_pres[i]); tb_pres[i] = malloc((size_t)tb_gw * tb_gh * 4); }
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
    if (tb_mac_cur) tb_mac_cursor_apply();
    while (1) {
        id ev = ((id(*)(id, SEL, unsigned long long, id, id, BOOL))objc_msgSend)(tb_nsapp,
            sel_getUid("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0ULL, 0, mode, YES);
        if (!ev) break;
        unsigned long t = TB_MSG(unsigned long, ev, "type");
        if (t == 1) tb_mdown = tb_mtap = 1;
        else if (t == 2) tb_mdown = 0;
        if (t == 1 || t == 2 || t == 5 || t == 6) {
            CGPoint p = TB_MSG(CGPoint, ev, "locationInWindow");
            tb_mx = (int)(p.x * tb_sc); tb_my = (int)(tb_gh - p.y * tb_sc);
        }
        if (t == 10 || t == 11) {
            unsigned short k = TB_MSG(unsigned short, ev, "keyCode");
            if (k < 128 && tb_mac_keys[k]) { tb_keys[tb_mac_keys[k]] = t == 10; if (t == 10) tb_ktap[tb_mac_keys[k]] = 1; }
            if (t == 10 && !(TB_MSG(unsigned long, ev, "modifierFlags") & ((1UL << 20) | (1UL << 18)))) {   // без Cmd и Ctrl
                id chars = TB_MSG(id, ev, "characters");
                if (chars) tb_type_utf8(TB_MSG(const char *, chars, "UTF8String"));
            }
            continue;   // клавиши не отдаём окну: иначе Mac пищит
        }
        if (t == 22) tb_wheel_add(TB_MSG(double, ev, "deltaY"));   // колёсико и два пальца на тачпаде
        if (t == 25 || t == 26) {   // другие кнопки мыши: 3 — назад, 4 — вперёд
            long n = TB_MSG(long, ev, "buttonNumber");
            if (n == 3 || n == 4) tb_side((int)n - 3, t == 25);
        }
        if (t == 12) {  // Shift
            unsigned long m = TB_MSG(unsigned long, ev, "modifierFlags");
            tb_keys[TB_K_SHIFT] = (m & (1UL << 17)) != 0;
            int was_cmd = tb_keys[TB_K_CMD], was_ctrl = tb_keys[TB_K_CTRL], was_alt = tb_keys[TB_K_ALT];
            tb_keys[TB_K_CTRL] = (m & (1UL << 18)) != 0;
            tb_keys[TB_K_ALT] = (m & (1UL << 19)) != 0;   // Option
            tb_keys[TB_K_CMD] = (m & (1UL << 20)) != 0;   // Command
            if (tb_keys[TB_K_CMD] && !was_cmd) tb_ktap[TB_K_CMD] = 1;
            if (tb_keys[TB_K_CTRL] && !was_ctrl) tb_ktap[TB_K_CTRL] = 1;
            if (tb_keys[TB_K_ALT] && !was_alt) tb_ktap[TB_K_ALT] = 1;
        }
        TB_MSG1(void, tb_nsapp, "sendEvent:", id, ev);
    }
    if (tb_resizable) {   // размер окна в точках (на Retina точка = 2×2 пикселя экрана)
        CGRect r;
        id view = TB_MSG(id, tb_wnd, "contentView");
#if defined(__x86_64__)
        ((void (*)(CGRect *, id, SEL))objc_msgSend_stret)(&r, view, sel_getUid("bounds"));
#else
        r = TB_MSG(CGRect, view, "bounds");
#endif
        int w = (int)(r.size.width * tb_sc + 0.5), h = (int)(r.size.height * tb_sc + 0.5);
        if (w > 0 && h > 0 && (w != tb_gw || h != tb_gh)) { tb_new_w = w; tb_new_h = h; }
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
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return TB_K_CTRL;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return TB_K_ALT;
    case VK_DELETE: return TB_K_DEL; case VK_HOME: return TB_K_HOME; case VK_END: return TB_K_END;
    case VK_PRIOR: return TB_K_PGUP; case VK_NEXT: return TB_K_PGDN;
    case VK_OEM_4: return '['; case VK_OEM_6: return ']'; case VK_OEM_PERIOD: return '.'; case VK_OEM_COMMA: return ',';
    case VK_OEM_MINUS: return '-'; case VK_OEM_PLUS: return '='; case VK_OEM_2: return '/'; case VK_OEM_1: return ';';
    case VK_OEM_7: return '\''; case VK_OEM_3: return '`'; case VK_OEM_5: return '\\';
    }
    if (v >= VK_F1 && v <= VK_F12) return TB_K_F1 + (int)(v - VK_F1);
    return 0;
}
TB_API void tb_win_blit(HDC dc) {
    StretchDIBits(dc, 0, 0, tb_gw, tb_gh, 0, 0, tb_gw, tb_gh, tb_px, &tb_bmi, DIB_RGB_COLORS, SRCCOPY);
}
TB_API int tb_win_cur;
TB_API LRESULT CALLBACK tb_win_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: tb_gclosed = 1; return 0;   // окно закроет сама программа: window_alive() вернёт false
    case WM_SETCURSOR:   // над окном — курсор, который выбрала программа (set_cursor)
        if (LOWORD(lp) == HTCLIENT) { tb_gfx_cursor(tb_win_cur); return TRUE; }
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        int k = tb_win_key(wp);
        if (k) { tb_keys[k] = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN; if (tb_keys[k]) tb_ktap[k] = 1; }
        if (wp != VK_F4) return 0;   // Alt+F4 пусть закрывает окно
        break;
    }
    case WM_MOUSEMOVE: tb_mx = (short)LOWORD(lp); tb_my = (short)HIWORD(lp); return 0;
    case WM_MOUSEWHEEL: tb_wheel_add((short)HIWORD(wp) / 120.0); return 0;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: tb_side(HIWORD(wp) == 2, msg == WM_XBUTTONDOWN); return TRUE;
    case WM_CHAR: {   // буква из TranslateMessage, в UTF-16: русские тоже, пары для смайликов
        static unsigned hi = 0;
        unsigned u = (unsigned)wp;
        if (u >= 0xD800 && u < 0xDC00) { hi = u; return 0; }
        if (u >= 0xDC00 && u < 0xE000) { if (hi) tb_type_cp(0x10000 + ((hi - 0xD800) << 10) + (u - 0xDC00)); hi = 0; return 0; }
        tb_type_cp(u);
        return 0;
    }
    case WM_LBUTTONDOWN: tb_mdown = tb_mtap = 1; tb_mx = (short)LOWORD(lp); tb_my = (short)HIWORD(lp); SetCapture(h); return 0;
    case WM_LBUTTONUP: tb_mdown = 0; tb_mx = (short)LOWORD(lp); tb_my = (short)HIWORD(lp); ReleaseCapture(); return 0;
    case WM_KILLFOCUS: memset(tb_keys, 0, sizeof tb_keys); return 0;
    case WM_SIZE: if (wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp)) { tb_new_w = LOWORD(lp); tb_new_h = HIWORD(lp); } return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); if (tb_px && !tb_gpu_on) tb_win_blit(dc); EndPaint(h, &ps); return 0; }
    }
    return DefWindowProcW(h, msg, wp, lp);
}
TB_API void tb_gfx_resizable(void) {
    LONG_PTR st = GetWindowLongPtrW(tb_hwnd, GWL_STYLE);
    SetWindowLongPtrW(tb_hwnd, GWL_STYLE, st | WS_THICKFRAME | WS_MAXIMIZEBOX);
    SetWindowPos(tb_hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
}
TB_API void tb_gfx_reimage(void) { tb_bmi.bmiHeader.biWidth = tb_gw; tb_bmi.bmiHeader.biHeight = -tb_gh; }
// окно Windows для Vulkan
TB_API int tb_win_surface(void) {
    struct { uint32_t sType; const void *pNext; VkFlags flags; HINSTANCE inst; HWND hwnd; } ci = { 1000009000, 0, 0, GetModuleHandleW(0), tb_hwnd };
    VkResult (TBVK_CALL *mk)(VkInstance, const void *, const void *, VkSurfaceKHR *) =
        (VkResult (TBVK_CALL *)(VkInstance, const void *, const void *, VkSurfaceKHR *))tbvk_gipa(tb_vk.inst, "vkCreateWin32SurfaceKHR");
    return mk && mk(tb_vk.inst, &ci, 0, &tb_vk.surf) == 0;
}
// window_sharp на Windows: программа сама отвечает за масштаб (150%, 200%) — окно в настоящих точках
TB_API double tb_gfx_scale(void) {
    SetProcessDPIAware();
    HDC dc = GetDC(0);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(0, dc);
    return dpi / 96.0;
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
    tb_win_cur = tb_cursor_want;
    if (tb_gpu_want()) tb_gpu_on = tb_vk_init("VK_KHR_win32_surface", tb_win_surface);
    if (!tb_gpu_on) tb_gpu_say("процессор");
    return 1;
}
// буфер обмена Windows: текст в UTF-16
TB_API char *tb_gfx_clip_get(void) {
    if (!OpenClipboard(tb_hwnd)) return 0;
    char *out = 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = h ? (const wchar_t *)GlobalLock(h) : 0;
    if (w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
        if (n > 0 && (out = malloc((size_t)n))) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, 0, 0);
        GlobalUnlock(h);
    }
    CloseClipboard();
    return out;
}
TB_API void tb_gfx_clip_set(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, 0, 0);
    if (n <= 0 || !OpenClipboard(tb_hwnd)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(wchar_t));
    wchar_t *w = g ? (wchar_t *)GlobalLock(g) : 0;
    if (w) {
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}
TB_API int tb_win_cur = 0;
TB_API void tb_gfx_cursor(int k) {
    static const LPCTSTR ids[7] = { IDC_ARROW, IDC_HAND, IDC_IBEAM, IDC_SIZEALL, IDC_SIZEWE, IDC_SIZENS, IDC_WAIT };
    tb_win_cur = k;
    SetCursor(k == 7 ? 0 : LoadCursor(0, ids[k]));
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
#include <X11/Xatom.h>
TB_API Display *tb_dpy = 0;
TB_API Window tb_xw;
TB_API GC tb_xgc;
TB_API XImage *tb_ximg = 0;
TB_API Atom tb_wm_del;
TB_API void tb_x_hints(int w0, int h0, int w1, int h1) {
    XSizeHints *sh = XAllocSizeHints();
    if (!sh) return;
    sh->flags = PMinSize | PMaxSize;
    sh->min_width = w0; sh->min_height = h0; sh->max_width = w1; sh->max_height = h1;
    XSetWMNormalHints(tb_dpy, tb_xw, sh);
    XFree(sh);
}
TB_API void tb_gfx_resizable(void) { tb_x_hints(1, 1, 8192, 8192); }
TB_API void tb_gfx_reimage(void) {
    tb_ximg->data = 0;   // точки — это tb_px, их освобождает сама программа
    XDestroyImage(tb_ximg);
    int scr = DefaultScreen(tb_dpy);
    tb_ximg = XCreateImage(tb_dpy, DefaultVisual(tb_dpy, scr), 24, ZPixmap, 0, (char *)tb_px, tb_gw, tb_gh, 32, 0);
}
// окно X11 для Vulkan
TB_API int tb_x_surface(void) {
    struct { uint32_t sType; const void *pNext; VkFlags flags; Display *dpy; Window window; } ci = { 1000004000, 0, 0, tb_dpy, tb_xw };
    VkResult (TBVK_CALL *mk)(VkInstance, const void *, const void *, VkSurfaceKHR *) =
        (VkResult (TBVK_CALL *)(VkInstance, const void *, const void *, VkSurfaceKHR *))tbvk_gipa(tb_vk.inst, "vkCreateXlibSurfaceKHR");
    return mk && mk(tb_vk.inst, &ci, 0, &tb_vk.surf) == 0;
}
// window_sharp в X11: масштаб из настройки Xft.dpi (её ставят GNOME и KDE), иначе 1
TB_API double tb_gfx_scale(void) {
    if (!tb_dpy) tb_dpy = XOpenDisplay(0);
    if (!tb_dpy) return 1;
    const char *v = XGetDefault(tb_dpy, "Xft", "dpi");
    double dpi = v ? atof(v) : 96;
    return dpi > 0 ? dpi / 96.0 : 1;
}
TB_API int tb_gfx_open(const char *title) {
    if (!tb_dpy) tb_dpy = XOpenDisplay(0);
    if (!tb_dpy) return 0;
    int scr = DefaultScreen(tb_dpy);
    tb_xw = XCreateSimpleWindow(tb_dpy, RootWindow(tb_dpy, scr), 0, 0, tb_gw, tb_gh, 0, 0, 0);
    tb_xgc = XCreateGC(tb_dpy, tb_xw, 0, 0);
    XSelectInput(tb_dpy, tb_xw, ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
    tb_x_hints(tb_gw, tb_gh, tb_gw, tb_gh);   // размер окна не меняется, пока программа не спросит window_w()
    Xutf8SetWMProperties(tb_dpy, tb_xw, title, title, 0, 0, 0, 0, 0);
    tb_wm_del = XInternAtom(tb_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(tb_dpy, tb_xw, &tb_wm_del, 1);
    XkbSetDetectableAutoRepeat(tb_dpy, True, 0);
    XMapWindow(tb_dpy, tb_xw);
    tb_ximg = XCreateImage(tb_dpy, DefaultVisual(tb_dpy, scr), 24, ZPixmap, 0, (char *)tb_px, tb_gw, tb_gh, 32, 0);
    if (tb_gpu_want()) tb_gpu_on = tb_vk_init("VK_KHR_xlib_surface", tb_x_surface);
    if (!tb_gpu_on) tb_gpu_say("процессор");
    if (tb_cursor_want) tb_gfx_cursor(tb_cursor_want);
    return 1;
}
// знак клавиши → буква Unicode (русская раскладка в X11 — свои старые номера 0x6c0..0x6ff)
TB_API unsigned tb_x_uni(KeySym s) {
    static const unsigned short cyr[64] = { 0x44e, 0x430, 0x431, 0x446, 0x434, 0x435, 0x444, 0x433, 0x445, 0x438, 0x439, 0x43a, 0x43b, 0x43c, 0x43d, 0x43e, 0x43f, 0x44f, 0x440, 0x441, 0x442, 0x443, 0x436, 0x432, 0x44c, 0x44b, 0x437, 0x448, 0x44d, 0x449, 0x447, 0x44a, 0x42e, 0x410, 0x411, 0x426, 0x414, 0x415, 0x424, 0x413, 0x425, 0x418, 0x419, 0x41a, 0x41b, 0x41c, 0x41d, 0x41e, 0x41f, 0x42f, 0x420, 0x421, 0x422, 0x423, 0x416, 0x412, 0x42c, 0x42b, 0x417, 0x428, 0x42d, 0x429, 0x427, 0x42a };
    if ((s >= 0x20 && s <= 0x7e) || (s >= 0xa0 && s <= 0xff)) return (unsigned)s;
    if ((s & 0xff000000) == 0x01000000) return (unsigned)(s & 0xffffff);
    if (s >= 0x6c0 && s <= 0x6ff) return cyr[s - 0x6c0];
    if (s == 0x6a3) return 0x451;
    if (s == 0x6b3) return 0x401;
    return 0;
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
    case XK_Control_L: case XK_Control_R: return TB_K_CTRL;
    case XK_Alt_L: case XK_Alt_R: return TB_K_ALT;
    case XK_Delete: return TB_K_DEL; case XK_Home: return TB_K_HOME; case XK_End: return TB_K_END;
    case XK_Prior: return TB_K_PGUP; case XK_Next: return TB_K_PGDN;
    }
    if (s >= XK_F1 && s <= XK_F12) return TB_K_F1 + (int)(s - XK_F1);
    if (s > 0x20 && s < 0x7f) return (int)s;   // знаки [ ] . , / - = : у X11 номер знака — это его код
    return 0;
}
// буфер обмена X11: текст держит сама программа-владелец и отдаёт его по просьбе (SelectionRequest)
TB_API char *tb_x_clip = 0;
TB_API void tb_x_clip_answer(XSelectionRequestEvent *r) {
    Atom utf8 = XInternAtom(tb_dpy, "UTF8_STRING", False), targets = XInternAtom(tb_dpy, "TARGETS", False);
    XSelectionEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SelectionNotify; ev.display = r->display; ev.requestor = r->requestor;
    ev.selection = r->selection; ev.target = r->target; ev.time = r->time; ev.property = None;
    Atom prop = r->property ? r->property : r->target;
    if (r->target == targets) {
        Atom list[3] = { targets, utf8, XA_STRING };
        XChangeProperty(tb_dpy, r->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, 3);
        ev.property = prop;
    } else if ((r->target == utf8 || r->target == XA_STRING) && tb_x_clip) {
        XChangeProperty(tb_dpy, r->requestor, prop, r->target, 8, PropModeReplace, (unsigned char *)tb_x_clip, (int)strlen(tb_x_clip));
        ev.property = prop;
    }
    XSendEvent(tb_dpy, r->requestor, False, 0, (XEvent *)&ev);
}
TB_API void tb_gfx_clip_set(const char *s) {
    free(tb_x_clip);
    tb_x_clip = strdup(s);
    if (!tb_dpy) return;
    XSetSelectionOwner(tb_dpy, XInternAtom(tb_dpy, "CLIPBOARD", False), tb_xw, CurrentTime);
    XFlush(tb_dpy);
}
TB_API char *tb_gfx_clip_get(void) {
    if (!tb_dpy) return tb_x_clip ? strdup(tb_x_clip) : 0;
    Atom clip = XInternAtom(tb_dpy, "CLIPBOARD", False), utf8 = XInternAtom(tb_dpy, "UTF8_STRING", False);
    Window owner = XGetSelectionOwner(tb_dpy, clip);
    if (owner == tb_xw) return tb_x_clip ? strdup(tb_x_clip) : 0;
    if (owner == None) return 0;
    Atom prop = XInternAtom(tb_dpy, "TACHBIT_CLIP", False);
    XConvertSelection(tb_dpy, clip, utf8, prop, tb_xw, CurrentTime);
    XFlush(tb_dpy);
    // ждём ответ владельца не дольше полсекунды; чужие события по дороге не теряем
    long long until = tb_time_ms() + 500;
    while (tb_time_ms() < until) {
        XEvent e;
        if (XCheckTypedWindowEvent(tb_dpy, tb_xw, SelectionNotify, &e)) {
            if (e.xselection.property == None) return 0;
            Atom type; int fmt; unsigned long n = 0, rest = 0; unsigned char *data = 0;
            XGetWindowProperty(tb_dpy, tb_xw, prop, 0, 1 << 24, True, AnyPropertyType, &type, &fmt, &n, &rest, &data);
            char *out = 0;
            if (data && fmt == 8) { out = malloc(n + 1); if (out) { memcpy(out, data, n); out[n] = 0; } }
            if (data) XFree(data);
            return out;
        }
        if (XCheckTypedEvent(tb_dpy, SelectionRequest, &e)) tb_x_clip_answer(&e.xselectionrequest);
        struct timespec ts = { 0, 2000000 };
        nanosleep(&ts, 0);
    }
    return 0;
}
TB_API void tb_gfx_cursor(int k) {
    static Cursor cache[8];
    static const unsigned shape[8] = { 68, 60, 152, 52, 108, 116, 150, 0 };   // XC_left_ptr, hand2, xterm, fleur, sb_h, sb_v, watch
    if (!cache[k]) {
        if (k == 7) {   // спрятать: пустая картинка 1×1
            static char none = 0;
            Pixmap pm = XCreateBitmapFromData(tb_dpy, tb_xw, &none, 1, 1);
            XColor black; memset(&black, 0, sizeof black);
            cache[k] = XCreatePixmapCursor(tb_dpy, pm, pm, &black, &black, 0, 0);
            XFreePixmap(tb_dpy, pm);
        } else cache[k] = XCreateFontCursor(tb_dpy, shape[k]);
    }
    XDefineCursor(tb_dpy, tb_xw, cache[k]);
    XFlush(tb_dpy);
}
TB_API void tb_gfx_events(void) {
    if (tb_gpu_on) tb_gpu_flush(1);
    else XPutImage(tb_dpy, tb_xw, tb_xgc, tb_ximg, 0, 0, 0, 0, tb_gw, tb_gh);
    while (XPending(tb_dpy)) {
        XEvent e; XNextEvent(tb_dpy, &e);
        if (e.type == SelectionRequest) tb_x_clip_answer(&e.xselectionrequest);
        else if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == tb_wm_del) tb_gclosed = 1;
        else if (e.type == KeyPress || e.type == KeyRelease) {
            int k = tb_x_key(XLookupKeysym(&e.xkey, 0));
            if (k) { tb_keys[k] = e.type == KeyPress; if (tb_keys[k]) tb_ktap[k] = 1; }
            if (e.type == KeyPress) {
                char buf[16]; KeySym ks = 0;
                XLookupString(&e.xkey, buf, sizeof buf, &ks, 0);
                if (!(e.xkey.state & ControlMask)) tb_type_cp(tb_x_uni(ks));
            }
        } else if (e.type == ButtonPress || e.type == ButtonRelease) {
            if (e.xbutton.button == 1) { tb_mdown = e.type == ButtonPress; if (tb_mdown) tb_mtap = 1; }
            if (e.xbutton.button == 8 || e.xbutton.button == 9) tb_side(e.xbutton.button - 8, e.type == ButtonPress);
            if (e.type == ButtonPress && e.xbutton.button == 4) tb_wheel_add(1);
            if (e.type == ButtonPress && e.xbutton.button == 5) tb_wheel_add(-1);
            tb_mx = e.xbutton.x; tb_my = e.xbutton.y;
        } else if (e.type == MotionNotify) { tb_mx = e.xmotion.x; tb_my = e.xmotion.y; }
        else if (e.type == ConfigureNotify && (e.xconfigure.width != tb_gw || e.xconfigure.height != tb_gh)) {
            tb_new_w = e.xconfigure.width; tb_new_h = e.xconfigure.height;
        }
    }
    XFlush(tb_dpy);
}
#endif
// TB_GPU_CHECK: проба смешивания. Видеокарты считают смешивание по-своему (AMD на Mac — с малой
// точностью, ошибка зависит и от цвета буквы, и от фона), поэтому проба берёт все сочетания:
// цвет c, фон d, прозрачность a — 256 × 256 × 256 точек, кадр за кадром, и процессор дальше
// смешивает по этой таблице (tb_blend_lut[c][d][a]). Делается один раз, когда есть и окно, и шрифт.
// После пробы кадр видеокарты снова становится таким же, как у процессора (копия tb_px)
TB_API void tb_gpu_blend_probe(void) {
    if (!tb_gpu_on || !tb_gpu_check || tb_blend_cal || !tb_px || !tb_nfonts) return;
    uint8_t *lut = malloc((size_t)1 << 24);
    if (!lut) return;
    size_t n = (size_t)tb_gw * tb_gh, p0 = tb_imgpx_n, need = p0 + 3 * n;
    int n0 = tb_nimg;
    uint32_t *all = need <= 0xFFFFFFFFu ? realloc(tb_imgpx, need * 4) : 0;
    tb_image *list = realloc(tb_imgs, (size_t)(n0 + 3) * sizeof(tb_image));
    if (all) tb_imgpx = all;
    if (list) tb_imgs = list;
    if (!all || !list) { free(lut); return; }
    for (int k = 0; k < 3; k++) { tb_imgs[n0 + k].w = tb_gw; tb_imgs[n0 + k].h = tb_gh; tb_imgs[n0 + k].off = (uint32_t)(p0 + k * n); }
    tb_nimg = n0 + 3; tb_imgpx_n = need;
    uint32_t *keep = tb_imgpx + p0 + 2 * n;   // кадр процессора: вернуть его видеокарте после пробы
    for (size_t q = 0; q < n; q++) keep[q] = 0xFF000000u | tb_px[q];
    tb_gpu_probing = 1;
    int ok = 1;
    for (size_t from = 0; from < ((size_t)1 << 24) && ok; from += n) {
        uint32_t *bg = tb_imgpx + p0, *fg = bg + n;
        for (size_t q = 0; q < n; q++) {
            size_t i = (from + q) & 0xFFFFFF;
            uint32_t c = (uint32_t)(i >> 16), d = (uint32_t)(i >> 8) & 255, a = (uint32_t)i & 255;
            bg[q] = 0xFF000000u | d * 0x010101u;
            fg[q] = (a << 24) | c * 0x010101u;
        }
        tb_img_dirty = 1;
        tb_gpu_image(n0, 0, 0);
        tb_gpu_image_a(n0 + 1, 0, 0);
        tb_gpu_flush(1);
        if (!tb_gpu_back) { ok = 0; break; }
        for (size_t q = 0; q < n && from + q < ((size_t)1 << 24); q++) lut[from + q] = (uint8_t)(tb_gpu_back[q] & 255);
    }
    tb_gpu_image(n0 + 2, 0, 0);
    tb_gpu_flush(1);
    tb_gpu_probing = 0;
    tb_nimg = n0; tb_imgpx_n = p0; tb_img_dirty = 1;
    if (!ok) { free(lut); return; }
    tb_blend_lut = lut;
    tb_blend_cal = 1;
    const char *e = getenv("TB_GPU_INFO");
    if (e && e[0] == '1') {
        long long diff = 0;
        for (size_t i = 0; i < ((size_t)1 << 24); i++) {
            uint32_t c = (uint32_t)(i >> 16), d = (uint32_t)(i >> 8) & 255, a = (uint32_t)i & 255;
            uint32_t w = a == 0 ? d : (c * a + 127) / 255 + (d * (255 - a) + 127) / 255;
            diff += lut[i] != w;
        }
        fprintf(stderr, "TB_GPU_CHECK: смешивание на этой видеокарте не как округление к ближайшему: %lld из %d\n", diff, 1 << 24);
    }
}
// window_sharp(): окно в настоящих точках экрана (Retina, 150% и 200% на Windows). Размер в window_open
// остаётся в «обычных» точках, а кадр, window_w/h и мышь — в настоящих; window_scale() — во сколько раз
TB_API double tb_gfx_scale(void);   // своя у каждой системы
TB_API void tb_window_sharp(void) { if (!tb_px) tb_sharp = 1; }
TB_API double tb_window_scale(void) { return tb_sc; }
TB_API void tb_window_open(long long w, long long h, const char *title) {
    if (tb_px) { tb_err = "окно уже открыто"; return; }
    if (w < 1 || h < 1 || w > 8192 || h > 8192) { tb_err = "размер окна от 1 до 8192"; return; }
    if (tb_sharp) {
        tb_sc = tb_gfx_scale();
        if (tb_sc < 1) tb_sc = 1;
        if (tb_sc > 4) tb_sc = 4;
        w = (long long)(w * tb_sc + 0.5); h = (long long)(h * tb_sc + 0.5);
        if (w > 8192) w = 8192;
        if (h > 8192) h = 8192;
    }
    tb_gw = (int)w; tb_gh = (int)h;
    tb_px = calloc((size_t)tb_gw * tb_gh, 4);
    if (!tb_gfx_open(title)) { free(tb_px); tb_px = 0; tb_err = "не могу открыть окно: нет экрана"; return; }
    if (tb_gpu_on && tb_gpu_check) { atexit(tb_gpu_report); tb_gpu_blend_probe(); }
    tb_last_frame = tb_time_ms();
}
// показать кадр, принять клавиши и мышь, держать 60 кадров в секунду; false — окно закрыли
// окно поменяло размер: новый кадр, старое остаётся в левом верхнем углу, новое место — чёрное
TB_API void tb_apply_resize(void) {
    if (!tb_new_w) return;
    int nw = tb_new_w > 8192 ? 8192 : tb_new_w, nh = tb_new_h > 8192 ? 8192 : tb_new_h, ow = tb_gw, oh = tb_gh;
    tb_new_w = tb_new_h = 0;
    if (nw < 1 || nh < 1 || (nw == ow && nh == oh)) return;
    uint32_t *np = calloc((size_t)nw * nh, 4);
    if (!np) return;
    for (int y = 0; y < oh && y < nh; y++) memcpy(np + (size_t)y * nw, tb_px + (size_t)y * ow, (size_t)(ow < nw ? ow : nw) * 4);
    free(tb_px);
    tb_px = np; tb_gw = nw; tb_gh = nh;
    tb_gfx_reimage();
    if (tb_gpu_on && !tb_gpu_resize(ow, oh)) fprintf(stderr, "видеокарта: не получилось поменять размер кадра\n");
}
TB_API long long tb_window_w(void) { if (tb_px && !tb_resizable) { tb_resizable = 1; tb_gfx_resizable(); } return tb_gw; }
TB_API long long tb_window_h(void) { if (tb_px && !tb_resizable) { tb_resizable = 1; tb_gfx_resizable(); } return tb_gh; }
// частота кадров: -1 — как у экрана (по умолчанию), 0 — сколько успеет, n — n кадров в секунду (set_fps)
TB_API double tb_frame_ms_v = 16.0;
TB_API long long tb_fps_want = -1, tb_fps_v = 0, tb_fps_n = 0, tb_fps_t0 = 0, tb_last_us = 0;
TB_API void tb_set_fps(long long n) {
    if (n < 0) n = -1;
    tb_fps_want = n;
    int nosync = n == 0 || getenv("TB_FPS_FREE") != 0;
    if (nosync != tb_gpu_nosync) { tb_gpu_nosync = nosync; if (tb_gpu_on) tb_gpu_vsync(); }
}
TB_API double tb_frame_ms(void) { return tb_frame_ms_v; }
TB_API long long tb_fps(void) { return tb_fps_v ? tb_fps_v : tb_frame_ms_v > 0 ? (long long)(1000 / tb_frame_ms_v) : 0; }
// показать кадр, принять клавиши и мышь, держать частоту кадров; false — окно закрыли
TB_API int tb_window_alive(void) {
    if (!tb_px || tb_gclosed) return 0;
    tb_gfx_events();   // здесь показывается прошлый кадр
    tb_zclr = tb_gzclr = 1;   // новый кадр: глубина 3D начинается заново
    tb_apply_resize();
    // TB_FPS_FREE=1 — без ограничения (для замеров скорости), как set_fps(0)
    static int free_fps = -1;
    if (free_fps < 0) free_fps = getenv("TB_FPS_FREE") != 0;
    long long now = tb_time_us(), period = 0;
    if (!tb_last_us) tb_last_us = now;
    if (free_fps || tb_fps_want == 0) period = 0;
    else if (tb_fps_want > 0) period = 1000000 / tb_fps_want;
    // как у экрана: видеокарта сама ждёт обновления экрана (60 Гц — 60 кадров, 120 Гц — 120);
    // кадр пришёл быстрее 4 мс — экран частоту не держит, держим сами 60 кадров в секунду
    else if (!tb_gpu_on || now - tb_last_us < 4000) period = 16667;
    if (period && now - tb_last_us < period) { usleep((useconds_t)(period - (now - tb_last_us))); now = tb_time_us(); }
    tb_frame_ms_v = (now - tb_last_us) / 1000.0;
    tb_last_us = now;
    if (!tb_fps_t0) tb_fps_t0 = now;
    tb_fps_n++;
    if (now - tb_fps_t0 >= 1000000) { tb_fps_v = tb_fps_n * 1000000 / (now - tb_fps_t0); tb_fps_n = 0; tb_fps_t0 = now; }
    tb_last_frame = now / 1000;
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
// полупрозрачный цвет (rgba): в старшем байте — прозрачность 1..255 (0 — непрозрачный, как у rgb)
TB_INL uint32_t tb_mix(uint32_t d, uint32_t c) {
    uint32_t t = (c >> 24) & 255, a = 255 - t;
    uint32_t rb = ((c & 0xFF00FF) * a + (d & 0xFF00FF) * t + 0x800080) >> 8 & 0xFF00FF;
    uint32_t g = ((c & 0x00FF00) * a + (d & 0x00FF00) * t + 0x008000) >> 8 & 0x00FF00;
    return rb | g;
}
TB_API void tb_cpu_rect(long long x, long long y, long long w, long long h, long long c) {
    long long x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > tb_gw ? tb_gw : x + w, y1 = y + h > tb_gh ? tb_gh : y + h;
    if ((c >> 24) & 255) {
        for (long long j = y0; j < y1; j++) for (long long i = x0; i < x1; i++) tb_px[j * tb_gw + i] = tb_mix(tb_px[j * tb_gw + i], (uint32_t)c);
        return;
    }
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
            if (i * i + j * j <= r * r + r) {
                if ((c >> 24) & 255) { uint32_t *p = &tb_px[(cy + j) * tb_gw + cx + i]; *p = tb_mix(*p, (uint32_t)c); }
                else tb_put(cx + i, cy + j, c);
            }
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
// Треугольник. Правило точек одно для процессора и видеокарты (кадры совпадают точка в точку):
// точка (x, y) берётся по своему центру (x + 0.5, y + 0.5); углы обходятся так, что площадь > 0;
// для каждой стороны a→b: e = (bx-ax)*(2y+1-2ay) - (by-ay)*(2x+1-2ax); точка внутри, если e > 0
// для всех трёх, а если e == 0 — только на сторонах с by > ay или (by == ay и bx < ax).
// Так у двух соседних треугольников общая сторона рисуется ровно один раз.
// Углы дальше ±8192 точек: треугольник сначала обрезается рамкой вокруг окна (tb_poly_draw) —
// тогда в шейдере (32-битные числа) ничего не переполняется.
TB_INL long long tb_tri_clamp(long long v) { return v < -8192 ? -8192 : v > 8192 ? 8192 : v; }
// t = x1 y1 x2 y2 x3 y3; 0 — рисовать нечего (площадь 0 или целиком вне окна)
TB_INL int tb_tri_prep(long long *t) {
    for (int i = 0; i < 6; i++) t[i] = tb_tri_clamp(t[i]);
    long long a2 = (t[2] - t[0]) * (t[5] - t[1]) - (t[3] - t[1]) * (t[4] - t[0]);
    if (a2 == 0) return 0;
    if (a2 < 0) { long long x = t[2], y = t[3]; t[2] = t[4]; t[3] = t[5]; t[4] = x; t[5] = y; }
    long long x0 = t[0], x1 = t[0], y0 = t[1], y1 = t[1];
    for (int i = 2; i < 6; i += 2) {
        if (t[i] < x0) x0 = t[i];
        if (t[i] > x1) x1 = t[i];
        if (t[i + 1] < y0) y0 = t[i + 1];
        if (t[i + 1] > y1) y1 = t[i + 1];
    }
    return !(x1 < 0 || y1 < 0 || x0 > tb_gw || y0 > tb_gh);
}
TB_INL int tb_tri_edge(long long ax, long long ay, long long bx, long long by, long long x, long long y) {
    long long ex = bx - ax, ey = by - ay, e = ex * (2 * y + 1 - 2 * ay) - ey * (2 * x + 1 - 2 * ax);
    return e > 0 || (e == 0 && (ey > 0 || (ey == 0 && ex < 0)));
}
// глубина точки (x, y) по плоскости треугольника — так же, как в шейдере: 32-битные числа
// без знака (переполнение по кругу), потом сдвиг; 0 — у самой камеры, 16777215 — далеко
TB_INL uint32_t tb_tri3_depth(const uint32_t *pl, long long x, long long y) {
    uint32_t k = pl[2] + pl[0] * (uint32_t)(2 * x + 1) + pl[1] * (uint32_t)(2 * y + 1);
    int32_t d = (int32_t)k >> 6;
    return d < 0 ? 0u : d > 16777215 ? 16777215u : (uint32_t)d;
}
// треугольник с углами в пределах ±8192 (t меняется: порядок углов); pl — плоскость глубины (3D) или 0
// gpu, cpu — где рисовать (сетку на видеокарте может считать сама видеокарта: тогда здесь только процессор)
static void tb_tri_draw_x(long long *t, long long c, const uint32_t *pl, int gpu, int cpu) {
    if (!tb_tri_prep(t)) return;
    if (gpu) { if (pl) tb_gpu_tri3(t, c, pl); else tb_gpu_triangle(t, c); }
    if (cpu) {
        long long xa = t[0], xb = t[0], ya = t[1], yb = t[1];
        for (int i = 2; i < 6; i += 2) {
            if (t[i] < xa) xa = t[i];
            if (t[i] > xb) xb = t[i];
            if (t[i + 1] < ya) ya = t[i + 1];
            if (t[i + 1] > yb) yb = t[i + 1];
        }
        if (xa < 0) xa = 0;
        if (ya < 0) ya = 0;
        if (xb > tb_gw - 1) xb = tb_gw - 1;
        if (yb > tb_gh - 1) yb = tb_gh - 1;
        for (long long y = ya; y <= yb; y++)
            for (long long x = xa; x <= xb; x++)
                if (tb_tri_edge(t[0], t[1], t[2], t[3], x, y) && tb_tri_edge(t[2], t[3], t[4], t[5], x, y) &&
                    tb_tri_edge(t[4], t[5], t[0], t[1], x, y)) {
                    if (pl) {   // 3D: точка видна, если ближе того, что уже нарисовано
                        uint32_t d = tb_tri3_depth(pl, x, y);
                        if (d >= tb_zb[y * tb_gw + x]) continue;
                        tb_zb[y * tb_gw + x] = d;
                    }
                    tb_px[y * tb_gw + x] = (uint32_t)c;
                }
    }
}
static void tb_tri_draw_i(long long *t, long long c, const uint32_t *pl) { tb_tri_draw_x(t, c, pl, tb_gpu_on, TB_CPU_DRAW); }
// многоугольник с углами-дробями (3D после проекции, огромные треугольники): обрезаем рамкой
// вокруг окна (±2048 точек, но не дальше ±8192), углы округляем, режем веером из первого угла.
// Общие углы округляются одинаково, поэтому соседние треугольники стыкуются без щелей
// округление до ближайшего целого (половина — вверх); быстрее llround, одно и то же везде
TB_INL long long tb_rnd(double x) {
    long long r = (long long)(x + 0.5);
    if ((double)r > x + 0.5) r--;
    return r;
}
static void tb_poly_draw(double *px, double *py, int n, long long c, const uint32_t *pl) {
    double gx0 = -2048, gy0 = -2048, gx1 = tb_gw + 2048.0, gy1 = tb_gh + 2048.0;
    if (gx1 > 8192) gx1 = 8192;
    if (gy1 > 8192) gy1 = 8192;
    double ax[16], ay[16], bx[16], by[16];
    for (int i = 0; i < n; i++) { ax[i] = px[i]; ay[i] = py[i]; }
    for (int side = 0; side < 4 && n > 0; side++) {   // Сазерленд — Ходжман: по одной стороне рамки
        int m = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            double vi = side == 0 ? ax[i] - gx0 : side == 1 ? gx1 - ax[i] : side == 2 ? ay[i] - gy0 : gy1 - ay[i];
            double vj = side == 0 ? ax[j] - gx0 : side == 1 ? gx1 - ax[j] : side == 2 ? ay[j] - gy0 : gy1 - ay[j];
            if (vi >= 0) { bx[m] = ax[i]; by[m] = ay[i]; m++; }
            if ((vi >= 0) != (vj >= 0)) {
                double k = vi / (vi - vj);
                bx[m] = ax[i] + (ax[j] - ax[i]) * k; by[m] = ay[i] + (ay[j] - ay[i]) * k; m++;
            }
        }
        n = m;
        for (int i = 0; i < n; i++) { ax[i] = bx[i]; ay[i] = by[i]; }
    }
    if (n < 3) return;
    long long rx[16], ry[16];
    for (int i = 0; i < n; i++) { rx[i] = tb_rnd(ax[i]); ry[i] = tb_rnd(ay[i]); }
    for (int i = 1; i + 1 < n; i++) {
        long long t[6] = { rx[0], ry[0], rx[i], ry[i], rx[i + 1], ry[i + 1] };
        tb_tri_draw_i(t, c, pl);
    }
}
TB_API void tb_draw_triangle(long long x1, long long y1, long long x2, long long y2, long long x3, long long y3, long long c) {
    if (!tb_px) return;
    long long t[6] = { x1, y1, x2, y2, x3, y3 };
    int big = 0;   // (не far/near: на Windows это пустые слова из windows.h)
    for (int i = 0; i < 6; i++) if (t[i] < -8192 || t[i] > 8192) big = 1;
    if (!big) { tb_tri_draw_i(t, c, 0); return; }
    double px[3] = { (double)x1, (double)x2, (double)x3 }, py[3] = { (double)y1, (double)y2, (double)y3 };
    tb_poly_draw(px, py, 3, c, 0);
}
// картинка из load_image: левый верхний угол в (x, y); прозрачные точки (меньше 128) не рисуются
TB_API void tb_draw_image(long long id, long long x, long long y) {
    if (!tb_px || id < 0 || id >= tb_nimg) return;
    if (tb_gpu_on) tb_gpu_image(id, x, y);
    if (TB_CPU_DRAW) {
        tb_image *im = &tb_imgs[id];
        const uint32_t *src = tb_imgpx + im->off;
        long long ja = y < 0 ? -y : 0, jb = y + im->h > tb_gh ? tb_gh - y : im->h;
        long long ia = x < 0 ? -x : 0, ib = x + im->w > tb_gw ? tb_gw - x : im->w;
        for (long long j = ja; j < jb; j++) for (long long i = ia; i < ib; i++) {
            uint32_t v = src[j * im->w + i];
            if ((v >> 24) >= 128) tb_px[(y + j) * tb_gw + x + i] = v & 0xFFFFFF;
        }
    }
}
// то же с настоящей прозрачностью: точка = округл(цвет * a / 255) + округл(фон * (255 - a) / 255).
// Две части округляются порознь: так получается одинаково на любой видеокарте (шейдер считает первую часть сам)
TB_API void tb_draw_image_a(long long id, long long x, long long y) {
    if (!tb_px || id < 0 || id >= tb_nimg) return;
    if (tb_gpu_on) tb_gpu_image_a(id, x, y);
    if (TB_CPU_DRAW) {
        tb_image *im = &tb_imgs[id];
        const uint32_t *src = tb_imgpx + im->off;
        long long ja = y < 0 ? -y : 0, jb = y + im->h > tb_gh ? tb_gh - y : im->h;
        long long ia = x < 0 ? -x : 0, ib = x + im->w > tb_gw ? tb_gw - x : im->w;
        for (long long j = ja; j < jb; j++) for (long long i = ia; i < ib; i++) {
            uint32_t v = src[j * im->w + i], a = v >> 24;
            if (a == 0) continue;
            uint32_t *d = &tb_px[(y + j) * tb_gw + x + i];
            if (a == 255) { *d = v & 0xFFFFFF; continue; }
            uint32_t o = *d, r = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                uint32_t sc = (v >> sh) & 255, dc = (o >> sh) & 255;
                uint32_t q = tb_blend_cal ? tb_blend_lut[(sc << 16) | (dc << 8) | a] : (sc * a + 127) / 255 + (dc * (255 - a) + 127) / 255;
                r |= (q > 255 ? 255 : q) << sh;
            }
            *d = r;
        }
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
        if (u == 0xD7) u = 'x';   // × как x (размер окна 640×400)
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
// ---------- 3D: сетки и камера ----------
// Сетка — список треугольников в мире (x вправо, y вверх, z вперёд). draw_mesh поворачивает её
// вокруг вертикали, ставит в точку, смотрит камерой (угол обзора 60°), обрезает всё ближе 0.05
// и рисует. Ближнее закрывает дальнее: у каждой точки есть глубина (tb_zb на процессоре,
// буфер глубины на видеокарте). Углы, обрезка и плоскость глубины считаются на процессоре,
// а глубину каждой точки видеокарта и процессор считают одной и той же целой формулой:
// кадры совпадают точка в точку.
// сетка: свои углы без повторов (у куба 8, а не 36) и треугольники как три номера угла.
// Каждый угол поворачивается и проецируется один раз на draw_mesh, а не в каждом своём треугольнике
// vi — те же углы в целых (× 4096, по 4 числа: x, y, z, 0) для 3D на видеокарте; bb — коробка вокруг углов;
// big — есть угол дальше 262 144 от центра сетки (в целые не влезает: только старый путь);
// cull — какие грани не видны (0 — сетка не закрытая, считать все), cullok — cull уже посчитан;
// gv, gt — где углы и треугольники сетки в памяти видеокарты; gused — в какой пачке видеокарта её рисует
typedef struct { double *v; long long nv, capv; uint32_t *t, *c; long long n, cap; long long *hk; long long hcap;
    int32_t *vi; double bb[6]; int big, cull, cullok; long long gv, gt, gused;
    long long nch, *chr; double *chs, bs[4]; int chok; } tb_mesh;   // куски: chr — (первый треугольник, сколько), chs и bs — шары (центр, радиус)
TB_API tb_mesh *tb_meshes = 0;
TB_API long long tb_nmesh = 0, tb_capmesh = 0;
TB_API double tb_cam[9] = { 0, 0, -5,  1, 0, 0,  0, 1, 0 };   // где камера, вправо, вверх
TB_API double tb_camf[3] = { 0, 0, 1 };                      // вперёд
TB_API long long tb_mesh_new(void) {
    if (tb_nmesh == tb_capmesh) {
        tb_capmesh = tb_capmesh ? tb_capmesh * 2 : 16;
        tb_meshes = (tb_mesh *)realloc(tb_meshes, sizeof(tb_mesh) * tb_capmesh);
    }
    memset(&tb_meshes[tb_nmesh], 0, sizeof(tb_mesh));
    tb_meshes[tb_nmesh].gused = -1;
    return tb_nmesh++;
}
TB_INL uint64_t tb_vhash(const double *p) {
    uint64_t h = 1469598103934665603ULL, w[3];
    memcpy(w, p, 24);
    for (int i = 0; i < 3; i++) { h ^= w[i]; h *= 1099511628211ULL; h ^= h >> 29; }
    return h;
}
// номер угла (x, y, z) в сетке: такой же уже есть — тот же номер (таблица с открытой адресацией)
static uint32_t tb_mesh_vertex(tb_mesh *m, double x, double y, double z) {
    double p[3] = { x + 0.0, y + 0.0, z + 0.0 };   // + 0.0: -0 и 0 — один угол
    if (m->nv * 2 >= m->hcap) {
        long long nc = m->hcap ? m->hcap * 2 : 64;
        long long *nk = (long long *)malloc(sizeof(long long) * nc);
        for (long long i = 0; i < nc; i++) nk[i] = -1;
        for (long long v = 0; v < m->nv; v++) {
            uint64_t h = tb_vhash(m->v + 3 * v) & (uint64_t)(nc - 1);
            while (nk[h] >= 0) h = (h + 1) & (uint64_t)(nc - 1);
            nk[h] = v;
        }
        free(m->hk); m->hk = nk; m->hcap = nc;
    }
    uint64_t h = tb_vhash(p) & (uint64_t)(m->hcap - 1);
    while (m->hk[h] >= 0) {
        if (memcmp(m->v + 3 * m->hk[h], p, 24) == 0) return (uint32_t)m->hk[h];
        h = (h + 1) & (uint64_t)(m->hcap - 1);
    }
    if (m->nv == m->capv) {
        m->capv = m->capv ? m->capv * 2 : 16;
        m->v = (double *)realloc(m->v, sizeof(double) * 3 * m->capv);
        m->vi = (int32_t *)realloc(m->vi, sizeof(int32_t) * 4 * m->capv);
    }
    memcpy(m->v + 3 * m->nv, p, 24);
    for (int i = 0; i < 3; i++) {
        if (m->nv == 0 || p[i] < m->bb[i]) m->bb[i] = p[i];
        if (m->nv == 0 || p[i] > m->bb[3 + i]) m->bb[3 + i] = p[i];
        if (!(fabs(p[i]) < 262144)) m->big = 1;   // и NaN тоже
        m->vi[4 * m->nv + i] = m->big ? 0 : (int32_t)tb_rnd(p[i] * 4096);
    }
    m->vi[4 * m->nv + 3] = 0;
    m->hk[h] = m->nv;
    return (uint32_t)m->nv++;
}
TB_API void tb_mesh_add(long long id, double x1, double y1, double z1, double x2, double y2, double z2,
                        double x3, double y3, double z3, long long c) {
    if (id < 0 || id >= tb_nmesh) return;
    tb_mesh *m = &tb_meshes[id];
    // видеокарта ещё не нарисовала эту сетку из текущей пачки: сначала отдать пачку (там старая сетка)
    if (tb_gpu_on && m->gused == tb_gbatch) tb_gpu_flush(0);
    tb_mesh_gdirty = 1;
    m->cullok = 0;
    m->chok = 0;
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->t = (uint32_t *)realloc(m->t, sizeof(uint32_t) * 3 * m->cap);
        m->c = (uint32_t *)realloc(m->c, sizeof(uint32_t) * m->cap);
    }
    uint32_t *t = m->t + 3 * m->n;
    t[0] = tb_mesh_vertex(m, x1, y1, z1);
    t[1] = tb_mesh_vertex(m, x2, y2, z2);
    t[2] = tb_mesh_vertex(m, x3, y3, z3);
    m->c[m->n++] = (uint32_t)c;
}
TB_API void tb_camera(double x, double y, double z, double lx, double ly, double lz) {
    double f[3] = { lx - x, ly - y, lz - z }, l = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (l < 1e-12) return;   // смотреть в себя нельзя: камера не меняется
    for (int i = 0; i < 3; i++) f[i] /= l;
    double up[3] = { 0, 1, 0 };
    if (fabs(f[1]) > 0.9999) { up[1] = 0; up[2] = 1; }   // смотрим прямо вверх или вниз
    double r[3] = { up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0] };
    l = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (int i = 0; i < 3; i++) r[i] /= l;
    double u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    tb_cam[0] = x; tb_cam[1] = y; tb_cam[2] = z;
    for (int i = 0; i < 3; i++) { tb_cam[3 + i] = r[i]; tb_cam[6 + i] = u[i]; tb_camf[i] = f[i]; }
}
// плоскость глубины по трём точкам экрана (x, y, D); 0 — треугольник ребром к нам, рисовать нечего
TB_INL int tb_tri3_plane(const double *sx, const double *sy, const double *dd, uint32_t *pl) {
    double e1x = sx[1] - sx[0], e1y = sy[1] - sy[0], e2x = sx[2] - sx[0], e2y = sy[2] - sy[0];
    double area = e1x * e2y - e1y * e2x;
    if (fabs(area) < 0.5) return 0;   // смотрит на нас ребром: точек почти нет
    double ia = 1 / area;
    double ax = ((dd[1] - dd[0]) * e2y - (dd[2] - dd[0]) * e1y) * ia;
    double by = ((dd[2] - dd[0]) * e1x - (dd[1] - dd[0]) * e2x) * ia;
    if (ax > 1e9) ax = 1e9;
    if (ax < -1e9) ax = -1e9;
    if (by > 1e9) by = 1e9;
    if (by < -1e9) by = -1e9;
    double c0 = dd[0] - ax * sx[0] - by * sy[0];
    // в целые: точка берётся по центру (2x + 1) / 2, поэтому ax / 2; ещё × 64 для точности
    pl[0] = (uint32_t)tb_rnd(ax * 32); pl[1] = (uint32_t)tb_rnd(by * 32); pl[2] = (uint32_t)tb_rnd(c0 * 64);
    return 1;
}
// углы сетки после камеры: место перед камерой и на экране (округлённое), глубина D
typedef struct { double cx, cy, cz, sx, sy, dd; long long rx, ry; int front, near_ok; } tb_3v;
TB_API tb_3v *tb_3vs = 0;
TB_API long long tb_3vcap = 0;
// старый путь (дроби): сетка задевает камеру или край, где целые не годятся — обрезка у камеры и рамкой
TB_API long long *tb_3vst = 0, tb_3vgen = 0;   // какой угол уже посчитан в этом вызове
static void tb_draw_mesh_d(tb_mesh *m, double x, double y, double z, double turn, long long t0, long long t1) {
    if (m->nv > tb_3vcap) {
        tb_3vcap = m->nv * 2;
        tb_3vs = (tb_3v *)realloc(tb_3vs, sizeof(tb_3v) * tb_3vcap);
        tb_3vst = (long long *)realloc(tb_3vst, sizeof(long long) * tb_3vcap);
        memset(tb_3vst, 0, sizeof(long long) * tb_3vcap);
    }
    long long gen = ++tb_3vgen;
    double a = turn * 3.14159265358979323846 / 180, ca = cos(a), sa = sin(a);
    double f = tb_gh / 2.0 / tan(30 * 3.14159265358979323846 / 180), zn = 205.0 / 4096, hw = tb_gw / 2.0, hh = tb_gh / 2.0;
    double gx0 = -2048, gy0 = -2048, gx1 = tb_gw + 2048.0, gy1 = tb_gh + 2048.0;   // та же рамка, что в tb_poly_draw
    if (gx1 > 8192) gx1 = 8192;
    if (gy1 > 8192) gy1 = 8192;
    // 1) каждый угол нужных треугольников (один раз): поворот, место в мире, взгляд камеры, экран
    for (long long t = t0; t < t1; t++) for (int i = 0; i < 3; i++) {
        long long k = m->t[3 * t + i];
        if (tb_3vst[k] == gen) continue;
        tb_3vst[k] = gen;
        const double *p = m->v + 3 * k;
        tb_3v *v = &tb_3vs[k];
        double wx = p[0] * ca + p[2] * sa + x - tb_cam[0], wy = p[1] + y - tb_cam[1], wz = -p[0] * sa + p[2] * ca + z - tb_cam[2];
        v->cx = wx * tb_cam[3] + wy * tb_cam[4] + wz * tb_cam[5];
        v->cy = wx * tb_cam[6] + wy * tb_cam[7] + wz * tb_cam[8];
        v->cz = wx * tb_camf[0] + wy * tb_camf[1] + wz * tb_camf[2];
        v->front = v->cz >= zn;
        v->near_ok = 0;
        if (v->front) {
            double iz = 1 / v->cz;
            v->sx = hw + f * v->cx * iz;
            v->sy = hh - f * v->cy * iz;
            v->dd = (1 - zn * iz) * 16777216.0;   // D = (1 - zn / z) * 2^24 — по экрану меняется ровно по плоскости
            v->near_ok = v->sx >= gx0 && v->sx <= gx1 && v->sy >= gy0 && v->sy <= gy1;
            if (v->near_ok) { v->rx = tb_rnd(v->sx); v->ry = tb_rnd(v->sy); }
        }
    }
    // 2) треугольники
    for (long long t = t0; t < t1; t++) {
        const uint32_t *ti = m->t + 3 * t;
        tb_3v *v0 = &tb_3vs[ti[0]], *v1 = &tb_3vs[ti[1]], *v2 = &tb_3vs[ti[2]];
        uint32_t pl[3];
        if (v0->front && v1->front && v2->front) {
            double sx[3] = { v0->sx, v1->sx, v2->sx }, sy[3] = { v0->sy, v1->sy, v2->sy }, dd[3] = { v0->dd, v1->dd, v2->dd };
            if (!tb_tri3_plane(sx, sy, dd, pl)) continue;
            if (v0->near_ok && v1->near_ok && v2->near_ok) {   // обычный случай: целиком в рамке
                long long q[6] = { v0->rx, v0->ry, v1->rx, v1->ry, v2->rx, v2->ry };
                tb_tri_draw_i(q, m->c[t], pl);
            } else tb_poly_draw(sx, sy, 3, m->c[t], pl);
            continue;
        }
        // часть треугольника за камерой: обрезаем всё ближе zn — выходит 0, 3 или 4 угла
        tb_3v *vs[3] = { v0, v1, v2 };
        double qx[4], qy[4], qz[4];
        int n = 0;
        for (int i = 0; i < 3; i++) {
            const tb_3v *p = vs[i], *q = vs[(i + 1) % 3];
            if (p->front) { qx[n] = p->cx; qy[n] = p->cy; qz[n] = p->cz; n++; }
            if (p->front != q->front) {
                double k = (zn - p->cz) / (q->cz - p->cz);
                qx[n] = p->cx + (q->cx - p->cx) * k; qy[n] = p->cy + (q->cy - p->cy) * k; qz[n] = zn; n++;
            }
        }
        if (n < 3) continue;
        double sx[4], sy[4], dd[4];
        for (int i = 0; i < n; i++) {
            double iz = 1 / qz[i];
            sx[i] = hw + f * qx[i] * iz;
            sy[i] = hh - f * qy[i] * iz;
            dd[i] = (1 - zn * iz) * 16777216.0;
        }
        if (!tb_tri3_plane(sx, sy, dd, pl)) continue;
        tb_poly_draw(sx, sy, n, m->c[t], pl);
    }
}
// ---- 3D в целых числах: так же считает видеокарта (compiler/gpu/tb3.vert, tb.metal) ----
// деление с округлением вниз (b > 0); через числа без знака — одинаково на любой видеокарте
TB_INL long long tb_fdiv(long long a, long long b) {
    return a >= 0 ? (long long)((unsigned long long)a / (unsigned long long)b)
                  : -(long long)(((unsigned long long)(-a) + (unsigned long long)b - 1) / (unsigned long long)b);
}
// фокус экрана × 256 (угол обзора 60°)
TB_INL long long tb_m3_f(void) { return tb_rnd(tb_gh / 2.0 / tan(30 * 3.14159265358979323846 / 180) * 256); }
// угол сетки p (× 4096) по записи r: место на экране (целые точки) и глубина D = 2^24 - 2^24 * zn / z, zn = 205 / 4096
TB_INL void tb_m3_corner(const int32_t *r, const int32_t *p, long long f, long long *o) {
    long long c[3];
    for (int k = 0; k < 3; k++)
        c[k] = (((long long)r[4 * k] * p[0] + (long long)r[4 * k + 1] * p[1] + (long long)r[4 * k + 2] * p[2]) >> 30) + r[4 * k + 3];
    o[0] = ((long long)tb_gw * 128 + tb_fdiv(f * c[0], c[2]) + 128) >> 8;
    o[1] = ((long long)tb_gh * 128 - tb_fdiv(f * c[1], c[2]) + 128) >> 8;
    o[2] = 16777216 - tb_fdiv(205LL << 24, c[2]);
}
// треугольник a, b, c (x, y, D): 0 — рисовать нечего (ребром к нам или невидимая грань); иначе углы
// по кругу «как надо» в t и плоскость глубины в pl — то же, что tb_tri3_plane, только в целых
TB_INL int tb_m3_tri(const long long *a, const long long *b, const long long *c, int cull, long long *t, uint32_t *pl) {
    long long area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0 || (cull > 0 && area > 0) || (cull < 0 && area < 0)) return 0;
    if (area < 0) { const long long *k = b; b = c; c = k; area = -area; }
    long long e1x = b[0] - a[0], e1y = b[1] - a[1], e2x = c[0] - a[0], e2y = c[1] - a[1], d1 = b[2] - a[2], d2 = c[2] - a[2];
    long long A = tb_fdiv(64 * (d1 * e2y - d2 * e1y) + area, 2 * area), B = tb_fdiv(64 * (d2 * e1x - d1 * e2x) + area, 2 * area);
    long long C = 64 * a[2] - 2 * A * a[0] - 2 * B * a[1];
    pl[0] = (uint32_t)A; pl[1] = (uint32_t)B; pl[2] = (uint32_t)C;
    t[0] = a[0]; t[1] = a[1]; t[2] = b[0]; t[3] = b[1]; t[4] = c[0]; t[5] = c[1];
    return 1;
}
// закрытая сетка (каждая сторона треугольника есть и в обратную сторону у соседа): грань, повёрнутая
// от камеры, всегда закрыта другими гранями — и у куба снаружи, и у комнаты изнутри (грани внутрь).
// Повёрнута от камеры = на экране по часовой стрелке (площадь < 0). 0 — рисовать все, -1 — пропускать такие
static int tb_cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static int tb_mesh_cull(tb_mesh *m) {
    if (m->cullok) return m->cull;
    m->cullok = 1; m->cull = 0;
    long long ne = 3 * m->n;
    uint64_t *e = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)(ne ? ne : 1));
    double vol = 0;
    for (long long t = 0; t < m->n; t++) {
        const uint32_t *ti = m->t + 3 * t;
        for (int k = 0; k < 3; k++) e[3 * t + k] = ((uint64_t)ti[k] << 32) | ti[(k + 1) % 3];
        const double *p = m->v + 3 * ti[0], *q = m->v + 3 * ti[1], *r = m->v + 3 * ti[2];
        vol += p[0] * (q[1] * r[2] - q[2] * r[1]) + p[1] * (q[2] * r[0] - q[0] * r[2]) + p[2] * (q[0] * r[1] - q[1] * r[0]);
    }
    qsort(e, (size_t)ne, sizeof(uint64_t), tb_cmp_u64);
    int closed = ne > 0;
    for (long long i = 0; i < ne && closed; i++) {
        if (i + 1 < ne && e[i] == e[i + 1]) closed = 0;   // одна сторона дважды в одну сторону
        uint64_t back = (e[i] << 32) | (e[i] >> 32);
        if ((uint32_t)e[i] == (uint32_t)(e[i] >> 32) || !bsearch(&back, e, (size_t)ne, sizeof(uint64_t), tb_cmp_u64)) closed = 0;
    }
    free(e);
    if (closed && fabs(vol) > 1e-12) m->cull = -1;
    return m->cull;
}
// сетки для видеокарты: у каждой сначала углы (x, y, z, 0), потом треугольники (номера углов, цвет)
TB_API long long tb_mesh_pack(int32_t *dst) {
    long long at = 0;
    for (long long i = 0; i < tb_nmesh; i++) {
        tb_mesh *m = &tb_meshes[i];
        m->gv = at; m->gt = at + m->nv;
        if (dst) {
            memcpy(dst + 4 * at, m->vi, sizeof(int32_t) * 4 * (size_t)m->nv);
            for (long long t = 0; t < m->n; t++) {
                int32_t *o = dst + 4 * (m->gt + t);
                for (int k = 0; k < 3; k++) o[k] = (int32_t)(m->t[3 * t + k] + m->gv);
                o[3] = (int32_t)(m->c[t] & 0xFFFFFF);
            }
        }
        at += m->nv + m->n;
    }
    return at;
}
TB_API void tb_mesh_gpc(const tb_ginst *g, int slot, tb_gpc *pc) {
    tb_mesh *m = &tb_meshes[g->x0];
    pc->size[0] = (float)tb_gw; pc->size[1] = (float)tb_gh;
    pc->tbase = (int32_t)(m->gt + g->ox); pc->ntri = g->oy; pc->rbase = (slot * TB_GR_CAP + g->x1) * 3;
    pc->f = (int32_t)tb_m3_f(); pc->gw = tb_gw; pc->gh = tb_gh; pc->cull = g->y0;
}
TB_API long long *tb_3i = 0;   // углы сетки после камеры в целых: x, y, D
TB_API long long tb_3icap = 0;
// шар сетки или куска (центр в сетке, радиус) после камеры: 0 — не видно совсем (за камерой или вне экрана
// с запасом 2 точки), 1 — целые годятся (весь перед камерой, на экране не дальше ±8100 точек), 2 — старый путь
static int tb_m3_test(const double *sp, double M[3][3], const double *T, double f, int big) {
    double cc[3], rad = sp[3];
    for (int k = 0; k < 3; k++) cc[k] = M[k][0] * sp[0] + M[k][1] * sp[1] + M[k][2] * sp[2] + T[k];
    if (cc[2] + rad < 205.0 / 4096) return 0;
    double hw = tb_gw / 2.0 + 2, hh = tb_gh / 2.0 + 2, lx = sqrt(f * f + hw * hw), ly = sqrt(f * f + hh * hh);
    if ((f * cc[0] - hw * cc[2]) / lx > rad || (-f * cc[0] - hw * cc[2]) / lx > rad) return 0;
    if ((f * cc[1] - hh * cc[2]) / ly > rad || (-f * cc[1] - hh * cc[2]) / ly > rad) return 0;
    double zmin = cc[2] - rad;
    int ok = !big && zmin >= 0.12 && fabs(T[0]) < 400000 && fabs(T[1]) < 400000 && fabs(T[2]) < 400000 &&
             fabs(cc[0]) + rad < 400000 && fabs(cc[1]) + rad < 400000 && cc[2] + rad < 400000;
    if (ok) ok = tb_gw / 2.0 + f * (fabs(cc[0]) + rad) / zmin < 8100 && tb_gh / 2.0 + f * (fabs(cc[1]) + rad) / zmin < 8100;
    return ok ? 1 : 2;
}
// один draw_mesh в целых: запись (одна на вызов, в текущем наборе) и куски треугольников
typedef struct { tb_mesh *m; long long id; int cull; long long ri, rbatch; tb_grec rec; } tb_m3_go;
TB_API long long *tb_3ist = 0, tb_3igen = 0;   // какой угол уже посчитан в этом вызове (процессор)
static void tb_m3_draw(tb_m3_go *go, long long t0, long long tn) {
    tb_mesh *m = go->m;
    if (tb_gpu_on && tb_gpu_mesh) {   // видеокарта считает сама: только запись и «нарисуй треугольники t0.. из сетки»
        if (tb_gin == TB_GI_CAP || (go->rbatch != tb_gbatch && tb_grn == TB_GR_CAP)) tb_gpu_flush(0);
        if (go->rbatch != tb_gbatch) {   // запись этого вызова ещё не в текущем наборе
            go->ri = tb_grn++;
            go->rbatch = tb_gbatch;
            tb_grec_p[tb_gslot * TB_GR_CAP + go->ri] = go->rec;
        }
        tb_ginst *g = tb_gin ? &tb_gi[tb_gin - 1] : 0;
        if (g && g->m == 3u && g->x0 == (int32_t)go->id && g->y0 == go->cull && g->ox == t0 && g->oy == tn &&
            g->x1 + g->y1 == go->ri && (long long)(g->y1 + 1) * tn < 2000000000) g->y1++;   // та же сетка подряд: одним вызовом
        else tb_gpu_add(go->id, go->cull, go->ri, 1, t0, tn, 0, 3u);
        m->gused = tb_gbatch;
        if (!TB_CPU_DRAW) return;
    }
    // процессор: та же целая формула (кадр для сверки, или видеокарта без своего 3D, или без видеокарты)
    int gpu = tb_gpu_on && !tb_gpu_mesh, cpu = TB_CPU_DRAW;
    if (m->nv * 3 > tb_3icap) {
        tb_3icap = m->nv * 6;
        tb_3i = (long long *)realloc(tb_3i, sizeof(long long) * tb_3icap);
        tb_3ist = (long long *)realloc(tb_3ist, sizeof(long long) * tb_3icap / 3);
        memset(tb_3ist, 0, sizeof(long long) * (tb_3icap / 3));
    }
    long long fq = tb_m3_f(), gen = ++tb_3igen;
    for (long long t = t0; t < t0 + tn; t++) {
        const uint32_t *ti = m->t + 3 * t;
        for (int i = 0; i < 3; i++)
            if (tb_3ist[ti[i]] != gen) { tb_3ist[ti[i]] = gen; tb_m3_corner(go->rec.r, m->vi + 4 * ti[i], fq, tb_3i + 3 * ti[i]); }
        long long q[6];
        uint32_t pl[3];
        if (tb_m3_tri(tb_3i + 3 * ti[0], tb_3i + 3 * ti[1], tb_3i + 3 * ti[2], go->cull, q, pl)) tb_tri_draw_x(q, m->c[t], pl, gpu, cpu);
    }
}
// куски сетки: треугольники делятся пополам по самой длинной стороне, пока в куске не больше 256.
// Треугольники переставляются так, чтобы каждый кусок шёл подряд; у каждого куска свой шар
static int tb_kd_ax;
static const float *tb_kd_c;
static int tb_kd_cmp(const void *a, const void *b) {
    float x = tb_kd_c[3 * *(const uint32_t *)a + tb_kd_ax], y = tb_kd_c[3 * *(const uint32_t *)b + tb_kd_ax];
    return x < y ? -1 : x > y;
}
static void tb_kd(tb_mesh *m, uint32_t *ix, long long n, const float *cen, long long *at) {
    if (n <= 256) {
        long long c = m->nch++;
        m->chr[2 * c] = *at; m->chr[2 * c + 1] = n; *at += n;
        return;
    }
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (long long i = 0; i < n; i++) for (int k = 0; k < 3; k++) {
        float v = cen[3 * ix[i] + k];
        if (v < lo[k]) lo[k] = v;
        if (v > hi[k]) hi[k] = v;
    }
    int ax = 0;
    for (int k = 1; k < 3; k++) if (hi[k] - lo[k] > hi[ax] - lo[ax]) ax = k;
    tb_kd_ax = ax; tb_kd_c = cen;
    qsort(ix, (size_t)n, sizeof(uint32_t), tb_kd_cmp);
    tb_kd(m, ix, n / 2, cen, at);
    tb_kd(m, ix + n / 2, n - n / 2, cen, at);
}
static void tb_sphere(const tb_mesh *m, long long t0, long long tn, double *sp) {
    double lo[3] = { 1e300, 1e300, 1e300 }, hi[3] = { -1e300, -1e300, -1e300 }, r = 0;
    for (long long t = t0; t < t0 + tn; t++) for (int i = 0; i < 3; i++) {
        const double *p = m->v + 3 * m->t[3 * t + i];
        for (int k = 0; k < 3; k++) { if (p[k] < lo[k]) lo[k] = p[k]; if (p[k] > hi[k]) hi[k] = p[k]; }
    }
    for (int k = 0; k < 3; k++) { sp[k] = (lo[k] + hi[k]) / 2; r += (hi[k] - sp[k]) * (hi[k] - sp[k]); }
    sp[3] = sqrt(r) * (1 + 1e-9) + 1e-3;
}
static void tb_mesh_chunks(tb_mesh *m) {
    if (m->chok) return;
    m->chok = 1;
    free(m->chr); free(m->chs);
    m->chr = (long long *)malloc(sizeof(long long) * 2 * (size_t)(m->n / 64 + 2));
    m->nch = 0;
    long long at = 0;
    if (m->n > 256) {
        uint32_t *ix = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)m->n);
        float *cen = (float *)malloc(sizeof(float) * 3 * (size_t)m->n);
        for (long long t = 0; t < m->n; t++) {
            ix[t] = (uint32_t)t;
            for (int k = 0; k < 3; k++)
                cen[3 * t + k] = (float)((m->v[3 * m->t[3 * t] + k] + m->v[3 * m->t[3 * t + 1] + k] + m->v[3 * m->t[3 * t + 2] + k]) / 3);
        }
        tb_kd(m, ix, m->n, cen, &at);
        uint32_t *nt = (uint32_t *)malloc(sizeof(uint32_t) * 3 * (size_t)m->cap), *nc = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)m->cap);
        for (long long t = 0; t < m->n; t++) { memcpy(nt + 3 * t, m->t + 3 * ix[t], 12); nc[t] = m->c[ix[t]]; }
        free(m->t); free(m->c); m->t = nt; m->c = nc;
        free(ix); free(cen);
        tb_mesh_gdirty = 1;   // порядок треугольников поменялся: видеокарте залить заново
    } else if (m->n > 0) { m->chr[0] = 0; m->chr[1] = m->n; m->nch = 1; }
    m->chs = (double *)malloc(sizeof(double) * 4 * (size_t)(m->nch ? m->nch : 1));
    for (long long c = 0; c < m->nch; c++) tb_sphere(m, m->chr[2 * c], m->chr[2 * c + 1], m->chs + 4 * c);
    tb_sphere(m, 0, m->n, m->bs);
}
TB_API void tb_draw_mesh(long long id, double x, double y, double z, double turn) {
    if (!tb_px || id < 0 || id >= tb_nmesh) return;
    if (TB_CPU_DRAW && tb_zb_n != (long long)tb_gw * tb_gh) {   // глубина процессора: размер как у окна
        free(tb_zb);
        tb_zb_n = (long long)tb_gw * tb_gh;
        tb_zb = (uint32_t *)malloc((size_t)tb_zb_n * 4);
        tb_zclr = 1;
    }
    if (tb_zclr && TB_CPU_DRAW) { for (long long i = 0; i < tb_zb_n; i++) tb_zb[i] = 16777216u; }
    tb_zclr = 0;
    tb_mesh *m = &tb_meshes[id];
    if (m->n == 0) return;
    // поворот сетки и взгляд камеры — одна таблица M (3 × 3) и сдвиг T: угол p перед камерой = M p + T
    double a = turn * 3.14159265358979323846 / 180, ca = cos(a), sa = sin(a), M[3][3], T[3];
    double d[3] = { x - tb_cam[0], y - tb_cam[1], z - tb_cam[2] };
    for (int k = 0; k < 3; k++) {
        const double *row = k == 0 ? tb_cam + 3 : k == 1 ? tb_cam + 6 : tb_camf;
        M[k][0] = row[0] * ca - row[2] * sa; M[k][1] = row[1]; M[k][2] = row[0] * sa + row[2] * ca;
        T[k] = row[0] * d[0] + row[1] * d[1] + row[2] * d[2];
    }
    tb_mesh_chunks(m);
    int cull = tb_mesh_cull(m);
    double f = tb_gh / 2.0 / tan(30 * 3.14159265358979323846 / 180);
    int st = tb_m3_test(m->bs, M, T, f, m->big);
    if (st == 0) return;   // вся сетка за камерой или вне экрана
    tb_m3_go go = { m, id, cull, -1, -1, 0 };
    for (int k = 0; k < 3; k++) {
        for (int j = 0; j < 3; j++) go.rec.r[4 * k + j] = (int32_t)tb_rnd(M[k][j] * 1073741824.0);   // |M| <= 1: в 2^30 влезает
        go.rec.r[4 * k + 3] = fabs(T[k]) < 400000 ? (int32_t)tb_rnd(T[k] * 4096) : 0;   // дальше — только старый путь
    }
    if (st == 1) { tb_m3_draw(&go, 0, m->n); return; }
    if (m->nch <= 1) { tb_draw_mesh_d(m, x, y, z, turn, 0, m->n); return; }
    // сетка большая и задевает камеру или край: по кускам — дальние на видеокарте, ближние старым путём
    for (long long c = 0; c < m->nch; c++) {
        long long t0 = m->chr[2 * c], tn = m->chr[2 * c + 1];
        int cs = tb_m3_test(m->chs + 4 * c, M, T, f, m->big);
        if (cs == 1) tb_m3_draw(&go, t0, tn);
        else if (cs == 2) tb_draw_mesh_d(m, x, y, z, turn, t0, t0 + tn);
    }
}
static int tb_key_at(int k) { int d = tb_keys[k] || tb_ktap[k]; tb_ktap[k] = 0; return d; }
TB_API int tb_key_down(const char *name) {
    static const struct { const char *n; int k; } names[] = {
        {"left", TB_K_LEFT}, {"right", TB_K_RIGHT}, {"up", TB_K_UP}, {"down", TB_K_DOWN}, {"space", TB_K_SPACE},
        {"enter", TB_K_ENTER}, {"escape", TB_K_ESC}, {"backspace", TB_K_BACK}, {"tab", TB_K_TAB}, {"shift", TB_K_SHIFT},
#ifdef __APPLE__
        {"cmd", TB_K_CMD},        // Command на Mac
#else
        {"cmd", TB_K_CTRL},       // на Linux и Windows вместо Command — Ctrl: одна программа для всех
#endif
        {"ctrl", TB_K_CTRL}, {"alt", TB_K_ALT}, {"delete", TB_K_DEL}, {"home", TB_K_HOME}, {"end", TB_K_END},
        {"pageup", TB_K_PGUP}, {"pagedown", TB_K_PGDN},
        {"f1", TB_K_F1}, {"f2", TB_K_F1 + 1}, {"f3", TB_K_F1 + 2}, {"f4", TB_K_F1 + 3}, {"f5", TB_K_F1 + 4}, {"f6", TB_K_F1 + 5},
        {"f7", TB_K_F1 + 6}, {"f8", TB_K_F1 + 7}, {"f9", TB_K_F1 + 8}, {"f10", TB_K_F1 + 9}, {"f11", TB_K_F1 + 10}, {"f12", TB_K_F1 + 11} };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) if (strcmp(name, names[i].n) == 0) return tb_key_at(names[i].k);
    if (name[0] && !name[1]) {
        int ch = (unsigned char)name[0];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        return tb_key_at(ch);
    }
    return 0;
}
TB_API long long tb_mouse_x(void) { return tb_mx; }
TB_API long long tb_mouse_y(void) { return tb_my; }
TB_API int tb_mouse_down(void) { int d = tb_mdown || tb_mtap; tb_mtap = 0; return d; }
TB_API long long tb_mouse_wheel(void) { int w = tb_wheel; tb_wheel = 0; return w; }
TB_API int tb_mouse_back(void) { int d = tb_mside[0] || tb_mside_tap[0]; tb_mside_tap[0] = 0; return d; }
TB_API int tb_mouse_forward(void) { int d = tb_mside[1] || tb_mside_tap[1]; tb_mside_tap[1] = 0; return d; }
TB_API const char *tb_typed_text(void) {
    char *s = tb_tmem((size_t)tb_ntyped + 1);
    memcpy(s, tb_typed, (size_t)tb_ntyped);
    s[tb_ntyped] = 0;
    tb_ntyped = 0;
    return s;
}
// буфер обмена системы: текст, который копируют и вставляют между программами (Cmd/Ctrl+C, V)
TB_API const char *tb_clipboard_get(void) {
    char *got = tb_gfx_clip_get();   // malloc или 0
    size_t n = got ? strlen(got) : 0;
    char *s = tb_tmem(n + 1);
    if (n) memcpy(s, got, n);
    s[n] = 0;
    free(got);
    return s;
}
TB_API void tb_clipboard_set(const char *s) { tb_gfx_clip_set(s ? s : ""); }
// вид стрелки мыши над окном
TB_API void tb_set_cursor(const char *name, int line) {
    static const char *names[] = { "arrow", "hand", "text", "move", "resize_x", "resize_y", "wait", "none" };
    for (int i = 0; i < 8; i++)
        if (strcmp(name, names[i]) == 0) { if (tb_px) tb_gfx_cursor(i); else tb_cursor_want = i; return; }
    char buf[200];
    snprintf(buf, sizeof buf, "set_cursor: нет курсора «%s»; есть arrow, hand, text, move, resize_x, resize_y, wait, none", name);
    tb_crash(line, buf);
}
#endif
// ---------- для файла .ll (TB_LL): библиотека видна снаружи через tb_x_* ----------
// Сама библиотека — static: так её лучше встраивать. Эти обёртки — её «дверь» для LLVM.
// Без clang (llvm-link + opt + llc) библиотека заранее лежит в файле .bc, а программа
// целиком в .ll: показ значений, конструкторы и main тоже пишутся в .ll.
#if defined(TB_SND) && !defined(TB_BARE)
// ---------- звук: load_sound, play_sound, play_sound_at, listener ... ----------
// Микшер: 48 000 кадров в секунду, стерео, числа float, блоки по 10 мс.
// Голос = звук + громкость + высота (+ место в мире для 3D). В 3D: громкость падает с
// расстоянием, сторона (лево/право) слышна по громкости И по задержке между ушами (как у
// настоящей головы), дальнее ухо и звук сзади глуше (тень головы), воздух глушит высокие
// у далёких звуков, а движение даёт эффект Доплера. Громкость и место меняются плавно
// внутри блока — без щелчков. На выходе ограничитель: сколько звуков ни играй, не трещит.
// Куда идёт звук: Mac — AudioQueue, Linux — ALSA, Windows — waveOut (библиотеки грузятся при
// запуске; нет их — тишина, программа работает). TB_SOUND_FILE=путь.wav — в файл (проверки),
// TB_SOUND=0 — никуда (время идёт, звуки «играют» молча).
#ifndef _WIN32
#include <dlfcn.h>
#endif
#define TB_SR 48000
#define TB_SBLOCK 480
#define TB_NVOICE 64
typedef struct { float *d; long long n; int ch; double rate; int temp; } tb_snd;
typedef struct {
    int on, gen, snd, loop, is3d, fresh, stop;
    double pos, pitch, vol, x, y, z;
    float gl, gr, lpl, lpr;
    double dl, dr, lastd, dop;
} tb_voice;
static tb_snd *tb_snds;
static int tb_nsnd;
static tb_voice tb_voices[TB_NVOICE];
static pthread_mutex_t tb_slock = PTHREAD_MUTEX_INITIALIZER;
// слушатель: где он, вектор «вправо», вектор «вперёд» (как у camera)
static double tb_lis[9] = { 0, 0, 0, 1, 0, 0, 0, 0, 1 };
static double tb_master = 1;
static float tb_mg = 1, tb_lim = 1;
static int tb_snd_started;

TB_INL float tb_sget(const tb_snd *s, long long i, int c, int loop) {
    if (i < 0 || i >= s->n) {
        if (!loop || s->n == 0) return 0;
        i %= s->n;
        if (i < 0) i += s->n;
    }
    return s->d[i * s->ch + (c < s->ch ? c : 0)];
}
// точка между кадрами звука: кривая Эрмита по 4 соседям (чище, чем прямая между двумя)
TB_INL float tb_sread(const tb_snd *s, double p, int c, int loop) {
    double fl = floor(p);
    long long i = (long long)fl;
    float t = (float)(p - fl);
    float a = tb_sget(s, i - 1, c, loop), b = tb_sget(s, i, c, loop), cc = tb_sget(s, i + 1, c, loop), d = tb_sget(s, i + 2, c, loop);
    float c1 = 0.5f * (cc - a), c2 = a - 2.5f * b + 2 * cc - 0.5f * d, c3 = 0.5f * (d - a) + 1.5f * (b - cc);
    return ((c3 * t + c2) * t + c1) * t + b;
}
TB_INL float tb_smono(const tb_snd *s, double p, int loop) {
    if (s->ch > 1) return 0.5f * (tb_sread(s, p, 0, loop) + tb_sread(s, p, 1, loop));
    return tb_sread(s, p, 0, loop);
}
// голос кончился: временный звук (play_tone) больше не нужен
static void tb_voice_end(tb_voice *v) {
    v->on = 0;
    tb_snd *s = &tb_snds[v->snd];
    if (s->temp && s->d) { free(s->d); s->d = 0; s->n = 0; }
}
// куда и как громко слышен 3D-голос: громкость ушей, задержка ушей, фильтры, Доплер
static void tb_voice_3d(tb_voice *v, float *tl, float *tr, double *tdl, double *tdr, float *fl, float *fr) {
    double dx = v->x - tb_lis[0], dy = v->y - tb_lis[1], dz = v->z - tb_lis[2];
    double d = sqrt(dx * dx + dy * dy + dz * dz);
    double side = 0, front = 1;
    if (d > 1e-9) {
        side = (dx * tb_lis[3] + dy * tb_lis[4] + dz * tb_lis[5]) / d;
        front = (dx * tb_lis[6] + dy * tb_lis[7] + dz * tb_lis[8]) / d;
    }
    if (d < 0.25) { side *= d / 0.25; front = 1; }   // звук «в голове» — по центру
    double g = d <= 1 ? 1 : 1 / d;                    // громкость падает с расстоянием
    double a = (side + 1) * 0.7853981633974483;      // стороны с равной мощностью
    *tl = (float)(v->vol * g * cos(a) * 1.4142135623730951);
    *tr = (float)(v->vol * g * sin(a) * 1.4142135623730951);
    double itd = 0.00066 * TB_SR * fabs(side);        // до 0,66 мс между ушами
    *tdl = side > 0 ? itd : 0;
    *tdr = side < 0 ? itd : 0;
    double fc = 20000 / (1 + d * 0.03);               // воздух: далёкие звуки глуше
    if (front < 0) fc *= 1 - 0.35 * -front;           // сзади глуше
    double fs = fc * (1 - 0.65 * fabs(side));         // дальнее ухо в тени головы
    double cl = side > 0 ? fs : fc, cr = side < 0 ? fs : fc;
    *fl = (float)(1 - exp(-6.283185307179586 * cl / TB_SR));
    *fr = (float)(1 - exp(-6.283185307179586 * cr / TB_SR));
    // Доплер: как быстро звук приближается (расстояние за блок 10 мс), звук летит 343 ед/с
    double want = 1;
    if (!v->fresh) {
        double sp = (d - v->lastd) * TB_SR / TB_SBLOCK;
        if (fabs(sp) < 686) want = 343 / (343 + sp);   // быстрее — это прыжок, не движение
        if (want < 0.5) want = 0.5;
        if (want > 2) want = 2;
    }
    v->dop = v->fresh ? 1 : v->dop * 0.8 + want * 0.2;
    v->lastd = d;
}
// смешать n кадров всех голосов в out (стерео, по очереди лево-право)
static void tb_mix(float *out, int n) {
    memset(out, 0, sizeof(float) * 2 * (size_t)n);
    pthread_mutex_lock(&tb_slock);
    for (int k = 0; k < TB_NVOICE; k++) {
        tb_voice *v = &tb_voices[k];
        if (!v->on) continue;
        tb_snd *s = &tb_snds[v->snd];
        if (!s->d || s->n == 0) { tb_voice_end(v); continue; }
        float tl = (float)v->vol, tr = (float)v->vol, al = 1, ar = 1;
        double tdl = 0, tdr = 0;
        if (v->is3d) tb_voice_3d(v, &tl, &tr, &tdl, &tdr, &al, &ar);
        if (v->stop) { tl = 0; tr = 0; }
        if (v->fresh) { v->gl = tl; v->gr = tr; v->dl = tdl; v->dr = tdr; v->fresh = 0; }
        double step = s->rate / TB_SR * v->pitch * (v->is3d ? v->dop : 1);
        float gl = v->gl, gr = v->gr, dgl = (tl - gl) / n, dgr = (tr - gr) / n;
        double dl = v->dl, dr = v->dr, ddl = (tdl - dl) / n, ddr = (tdr - dr) / n;
        int done = 0;
        for (int i = 0; i < n; i++) {
            float l, r;
            if (v->is3d) {
                l = tb_smono(s, v->pos - dl * step, v->loop);
                r = tb_smono(s, v->pos - dr * step, v->loop);
                v->lpl += al * (l - v->lpl);
                v->lpr += ar * (r - v->lpr);
                l = v->lpl;
                r = v->lpr;
            } else if (s->ch > 1) {
                l = tb_sread(s, v->pos, 0, v->loop);
                r = tb_sread(s, v->pos, 1, v->loop);
            } else {
                l = r = tb_sread(s, v->pos, 0, v->loop);
            }
            out[2 * i] += l * gl;
            out[2 * i + 1] += r * gr;
            gl += dgl; gr += dgr; dl += ddl; dr += ddr;
            v->pos += step;
            if (v->pos >= (double)s->n) {
                if (v->loop) v->pos -= (double)s->n;
                else { done = 1; break; }
            }
        }
        v->gl = tl; v->gr = tr; v->dl = tdl; v->dr = tdr;
        if (done || v->stop) tb_voice_end(v);
    }
    float mt = (float)tb_master;
    pthread_mutex_unlock(&tb_slock);
    // общая громкость (плавно) и ограничитель: громко — сразу тише, потом медленно назад
    float dm = (mt - tb_mg) / n;
    for (int i = 0; i < n; i++) {
        tb_mg += dm;
        float l = out[2 * i] * tb_mg, r = out[2 * i + 1] * tb_mg;
        float pk = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        if (pk * tb_lim > 0.98f) tb_lim = 0.98f / pk;
        else tb_lim += (1 - tb_lim) * 0.0002f;
        l *= tb_lim; r *= tb_lim;
        out[2 * i] = l > 1 ? 1 : l < -1 ? -1 : l;
        out[2 * i + 1] = r > 1 ? 1 : r < -1 ? -1 : r;
    }
}
static long long tb_snd_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
// без устройства: время идёт как настоящее, звук — в файл (TB_SOUND_FILE) или никуда
static void tb_snd_paced(FILE *f) {
    float buf[TB_SBLOCK * 2];
    short pcm[TB_SBLOCK * 2];
    unsigned long long bytes = 0;
    long long next = tb_snd_now_us();
    for (;;) {
        tb_mix(buf, TB_SBLOCK);
        if (f) {
            for (int i = 0; i < TB_SBLOCK * 2; i++) pcm[i] = (short)lrintf(buf[i] * 32767);
            fwrite(pcm, sizeof pcm, 1, f);
            bytes += sizeof pcm;
            // заголовок WAV сразу верный: программа может закончиться когда угодно
            unsigned int riff = (unsigned int)(36 + bytes), data = (unsigned int)bytes;
            fseek(f, 4, SEEK_SET); fwrite(&riff, 4, 1, f);
            fseek(f, 40, SEEK_SET); fwrite(&data, 4, 1, f);
            fseek(f, 0, SEEK_END);
            fflush(f);
        }
        next += 1000000LL * TB_SBLOCK / TB_SR;
        long long wait = next - tb_snd_now_us();
        if (wait > 0) usleep((useconds_t)wait);
        else if (wait < -200000) next = tb_snd_now_us();   // программа стояла (отладчик) — не догонять
    }
}
static FILE *tb_wav_open(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    unsigned char h[44] = { 'R','I','F','F', 36,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
        0x80,0xBB,0,0, 0x00,0xEE,0x02,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    fwrite(h, 44, 1, f);
    fflush(f);
    return f;
}
#if defined(__APPLE__)
// ---------- Mac: AudioQueue (сама зовёт нас за следующей порцией) ----------
typedef struct { double rate; uint32_t id, flags, bpp, fpp, bpf, ch, bits, res; } tb_asbd;
typedef struct { uint32_t cap; void *data; uint32_t size; void *user; uint32_t pcap; void *pdesc; uint32_t pcount; } tb_aqbuf;
typedef int32_t (*tb_aq_enq_t)(void *, tb_aqbuf *, uint32_t, const void *);
static tb_aq_enq_t tb_aq_enq;
static void tb_aq_cb(void *user, void *q, tb_aqbuf *b) {
    (void)user;
    tb_mix((float *)b->data, TB_SBLOCK);
    b->size = TB_SBLOCK * 8;
    tb_aq_enq(q, b, 0, 0);
}
static int tb_snd_device(void) {
    void *lib = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return 0;
    int32_t (*nout)(const tb_asbd *, void (*)(void *, void *, tb_aqbuf *), void *, void *, void *, uint32_t, void **) =
        (int32_t (*)(const tb_asbd *, void (*)(void *, void *, tb_aqbuf *), void *, void *, void *, uint32_t, void **))dlsym(lib, "AudioQueueNewOutput");
    int32_t (*alloc)(void *, uint32_t, tb_aqbuf **) = (int32_t (*)(void *, uint32_t, tb_aqbuf **))dlsym(lib, "AudioQueueAllocateBuffer");
    int32_t (*start)(void *, const void *) = (int32_t (*)(void *, const void *))dlsym(lib, "AudioQueueStart");
    tb_aq_enq = (tb_aq_enq_t)dlsym(lib, "AudioQueueEnqueueBuffer");
    if (!nout || !alloc || !start || !tb_aq_enq) return 0;
    tb_asbd f = { TB_SR, 0x6C70636D, 1 | 8, 8, 1, 8, 2, 32, 0 };   // 'lpcm', float, без дыр
    void *q = 0;
    // очередь без своего цикла событий: AudioQueue зовёт tb_aq_cb из своего потока
    if (nout(&f, tb_aq_cb, 0, 0, 0, 0, &q) != 0 || !q) return 0;
    for (int i = 0; i < 3; i++) {
        tb_aqbuf *b = 0;
        if (alloc(q, TB_SBLOCK * 8, &b) != 0 || !b) return 0;
        tb_aq_cb(0, q, b);
    }
    return start(q, 0) == 0;
}
#elif defined(_WIN32)
// ---------- Windows: waveOut (кольцо из 6 порций) ----------
#if !defined(_WIN64)
#define TB_WINAPI __stdcall
#else
#define TB_WINAPI
#endif
#pragma pack(push, 1)
typedef struct { uint16_t tag, ch; uint32_t rate, bps; uint16_t align, bits, cb; } tb_wfx;
#pragma pack(pop)
typedef struct tb_whdr { char *data; uint32_t len, rec; uintptr_t user; uint32_t flags, loops; struct tb_whdr *next; uintptr_t res; } tb_whdr;
static void *tb_snd_win(void *arg) {
    (void)arg;
    HMODULE lib = LoadLibraryA("winmm.dll");
    uint32_t (TB_WINAPI *wopen)(void **, uint32_t, const tb_wfx *, uintptr_t, uintptr_t, uint32_t) = 0;
    uint32_t (TB_WINAPI *wprep)(void *, tb_whdr *, uint32_t) = 0;
    uint32_t (TB_WINAPI *wwrite)(void *, tb_whdr *, uint32_t) = 0;
    if (lib) {
        wopen = (uint32_t (TB_WINAPI *)(void **, uint32_t, const tb_wfx *, uintptr_t, uintptr_t, uint32_t))(void *)GetProcAddress(lib, "waveOutOpen");
        wprep = (uint32_t (TB_WINAPI *)(void *, tb_whdr *, uint32_t))(void *)GetProcAddress(lib, "waveOutPrepareHeader");
        wwrite = (uint32_t (TB_WINAPI *)(void *, tb_whdr *, uint32_t))(void *)GetProcAddress(lib, "waveOutWrite");
    }
    void *h = 0;
    tb_wfx f = { 3, 2, TB_SR, TB_SR * 8, 8, 32, 0 };   // 3 = числа float
    if (!wopen || !wprep || !wwrite || wopen(&h, 0xFFFFFFFFu, &f, 0, 0, 0) != 0) { tb_snd_paced(0); return 0; }
    static float bufs[6][TB_SBLOCK * 2];
    static tb_whdr hd[6];
    for (int i = 0; i < 6; i++) {
        hd[i].data = (char *)bufs[i];
        hd[i].len = sizeof bufs[i];
        wprep(h, &hd[i], sizeof hd[i]);
        tb_mix(bufs[i], TB_SBLOCK);
        wwrite(h, &hd[i], sizeof hd[i]);
    }
    for (int i = 0;; i = (i + 1) % 6) {
        while (!(hd[i].flags & 1)) Sleep(1);   // 1 = эту порцию уже сыграли
        tb_mix(bufs[i], TB_SBLOCK);
        wwrite(h, &hd[i], sizeof hd[i]);
    }
    return 0;
}
static int tb_snd_device(void) {
    pthread_t t;
    return pthread_create(&t, 0, tb_snd_win, 0) == 0;
}
#else
// ---------- Linux: ALSA (запись ждёт, пока устройство примет — она и задаёт темп) ----------
static void *tb_snd_alsa(void *arg) {
    (void)arg;
    void *lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    int (*popen_)(void **, const char *, int, int) = 0;
    int (*setp)(void *, int, int, unsigned, unsigned, int, unsigned) = 0;
    long (*writei)(void *, const void *, unsigned long) = 0;
    int (*recover)(void *, int, int) = 0;
    if (lib) {
        popen_ = (int (*)(void **, const char *, int, int))dlsym(lib, "snd_pcm_open");
        setp = (int (*)(void *, int, int, unsigned, unsigned, int, unsigned))dlsym(lib, "snd_pcm_set_params");
        writei = (long (*)(void *, const void *, unsigned long))dlsym(lib, "snd_pcm_writei");
        recover = (int (*)(void *, int, int))dlsym(lib, "snd_pcm_recover");
    }
    void *pcm = 0;
    // 14 = float little-endian, 3 = кадры подряд, задержка 40 мс
    if (!popen_ || !setp || !writei || !recover || popen_(&pcm, "default", 0, 0) < 0 || setp(pcm, 14, 3, 2, TB_SR, 1, 40000) < 0) {
        tb_snd_paced(0);
        return 0;
    }
    float buf[TB_SBLOCK * 2];
    for (;;) {
        tb_mix(buf, TB_SBLOCK);
        long r = writei(pcm, buf, TB_SBLOCK);
        if (r < 0 && recover(pcm, (int)r, 1) < 0) { tb_snd_paced(0); return 0; }
    }
}
static int tb_snd_device(void) {
    pthread_t t;
    return pthread_create(&t, 0, tb_snd_alsa, 0) == 0;
}
#endif
static void *tb_snd_paced_run(void *arg) { tb_snd_paced((FILE *)arg); return 0; }
// первый звук: включить вывод (один раз)
static void tb_snd_start(void) {
    if (tb_snd_started) return;
    tb_snd_started = 1;
    const char *file = getenv("TB_SOUND_FILE"), *off = getenv("TB_SOUND");
    pthread_t t;
    if (file && *file) {
        FILE *f = tb_wav_open(file);
        pthread_create(&t, 0, tb_snd_paced_run, f);
        return;
    }
    if ((off && strcmp(off, "0") == 0) || !tb_snd_device()) pthread_create(&t, 0, tb_snd_paced_run, 0);
}
TB_INL uint32_t tb_le16(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8; }
TB_INL uint32_t tb_le32(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
// новый звук в таблицу; номер или -1
static long long tb_snd_add(float *d, long long n, int ch, double rate, int temp) {
    pthread_mutex_lock(&tb_slock);
    int at = -1;
    if (temp)
        for (int i = 0; i < tb_nsnd; i++)
            if (tb_snds[i].temp && !tb_snds[i].d) { at = i; break; }
    if (at < 0) {
        tb_snd *list = realloc(tb_snds, sizeof(tb_snd) * (size_t)(tb_nsnd + 1));
        if (!list) { pthread_mutex_unlock(&tb_slock); return -1; }
        tb_snds = list;
        at = tb_nsnd++;
    }
    tb_snds[at].d = d; tb_snds[at].n = n; tb_snds[at].ch = ch; tb_snds[at].rate = rate; tb_snds[at].temp = temp;
    pthread_mutex_unlock(&tb_slock);
    return at;
}
// WAV: PCM 8/16/24/32 бита или float 32; моно или стерео (больше каналов — берём первые два)
TB_API long long tb_load_sound(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { tb_err = "нет такого файла звука"; return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = sz > 0 ? malloc((size_t)sz) : 0;
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); tb_err = "не могу прочитать файл звука"; return -1; }
    fclose(f);
    if (sz < 12 || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) {
        tb_err = sz >= 4 && memcmp(b, "OggS", 4) == 0 ? "OGG пока не умею: сохрани звук как WAV" : "это не WAV-файл";
        free(b);
        return -1;
    }
    uint32_t tag = 0, ch = 0, rate = 0, bits = 0;
    const unsigned char *data = 0;
    uint32_t dlen = 0;
    long p = 12;
    while (p + 8 <= sz) {
        uint32_t n = tb_le32(b + p + 4);
        const unsigned char *c = b + p + 8;
        if ((long)n > sz - p - 8) n = (uint32_t)(sz - p - 8);   // последний кусок обрезан — берём что есть
        if (memcmp(b + p, "fmt ", 4) == 0 && n >= 16) {
            tag = tb_le16(c); ch = tb_le16(c + 2); rate = tb_le32(c + 4); bits = tb_le16(c + 14);
            if (tag == 0xFFFE && n >= 26) tag = tb_le16(c + 24);   // «расширенный» WAV: вид внутри
        } else if (memcmp(b + p, "data", 4) == 0) {
            data = c; dlen = n;
        }
        p += 8 + (long)n + (n & 1);
    }
    int okf = (tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (tag == 3 && (bits == 32 || bits == 64));
    if (!data || !okf || ch == 0 || rate == 0) {
        free(b);
        tb_err = !data ? "в WAV нет звука (нет куска data)" : "такой WAV не умею: нужен PCM 8/16/24/32 бита или float";
        return -1;
    }
    uint32_t bpf = ch * (bits / 8);
    long long frames = dlen / bpf;
    int oc = ch >= 2 ? 2 : 1;
    float *d = malloc(sizeof(float) * (size_t)(frames * oc + 1));
    if (!d) { free(b); tb_err = "не хватило памяти"; return -1; }
    for (long long i = 0; i < frames; i++)
        for (int k = 0; k < oc; k++) {
            const unsigned char *q = data + i * bpf + (uint32_t)k * (bits / 8);
            float v;
            if (tag == 3) {
                if (bits == 32) { float x; memcpy(&x, q, 4); v = x; }
                else { double x; memcpy(&x, q, 8); v = (float)x; }
            } else if (bits == 8) v = (q[0] - 128) / 128.0f;
            else if (bits == 16) v = (int16_t)tb_le16(q) / 32768.0f;
            else if (bits == 24) v = (float)((int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 8) / 8388608.0f;
            else v = (float)((int32_t)tb_le32(q) / 2147483648.0);
            d[i * oc + k] = v;
        }
    free(b);
    long long id = tb_snd_add(d, frames, oc, rate, 0);
    if (id < 0) { free(d); tb_err = "не хватило памяти"; return -1; }
    return id;
}
// свободный голос; заняты все — забираем самый тихий (лучше не тот, что играет по кругу)
static long long tb_voice_new(long long s, int loop, int is3d, double x, double y, double z) {
    tb_snd_start();
    pthread_mutex_lock(&tb_slock);
    if (s < 0 || s >= tb_nsnd || !tb_snds[s].d) { pthread_mutex_unlock(&tb_slock); return 0; }
    int k = -1;
    for (int i = 0; i < TB_NVOICE; i++)
        if (!tb_voices[i].on) { k = i; break; }
    if (k < 0) {
        float best = 1e30f;
        for (int pass = 0; pass < 2 && k < 0; pass++)
            for (int i = 0; i < TB_NVOICE; i++) {
                tb_voice *v = &tb_voices[i];
                if (pass == 0 && v->loop) continue;
                if (v->gl + v->gr < best) { best = v->gl + v->gr; k = i; }
            }
        tb_voice_end(&tb_voices[k]);
    }
    tb_voice *v = &tb_voices[k];
    int gen = v->gen + 1;
    memset(v, 0, sizeof *v);
    v->gen = gen; v->on = 1; v->fresh = 1; v->snd = (int)s; v->loop = loop; v->is3d = is3d;
    v->pitch = 1; v->vol = 1; v->x = x; v->y = y; v->z = z; v->dop = 1;
    long long id = (long long)gen * TB_NVOICE + k + 1;
    pthread_mutex_unlock(&tb_slock);
    return id;
}
// голос по номеру (под замком); старый номер (голос уже другой) — 0
static tb_voice *tb_voice_at(long long id) {
    if (id <= 0) return 0;
    long long k = (id - 1) % TB_NVOICE, gen = (id - 1) / TB_NVOICE;
    tb_voice *v = &tb_voices[k];
    return v->on && v->gen == gen && !v->stop ? v : 0;
}
TB_API long long tb_play_sound(long long s) { return tb_voice_new(s, 0, 0, 0, 0, 0); }
TB_API long long tb_loop_sound(long long s) { return tb_voice_new(s, 1, 0, 0, 0, 0); }
TB_API long long tb_play_sound_at(long long s, double x, double y, double z) { return tb_voice_new(s, 0, 1, x, y, z); }
TB_API long long tb_loop_sound_at(long long s, double x, double y, double z) { return tb_voice_new(s, 1, 1, x, y, z); }
// тон: синус hz герц на ms миллисекунд, мягкое начало и конец (без щелчков)
TB_API long long tb_play_tone(double hz, long long ms) {
    if (ms <= 0 || hz <= 0) return 0;
    if (ms > 60000) ms = 60000;
    long long n = ms * TB_SR / 1000;
    float *d = malloc(sizeof(float) * (size_t)(n + 1));
    if (!d) return 0;
    long long ramp = TB_SR / 200;
    if (ramp * 2 > n) ramp = n / 2;
    for (long long i = 0; i < n; i++) {
        double e = 1;
        if (i < ramp) e = (double)i / ramp;
        else if (i >= n - ramp) e = (double)(n - 1 - i) / ramp;
        d[i] = (float)(0.5 * e * sin(6.283185307179586 * hz * (double)i / TB_SR));
    }
    long long s = tb_snd_add(d, n, 1, TB_SR, 1);
    if (s < 0) { free(d); return 0; }
    long long v = tb_voice_new(s, 0, 0, 0, 0, 0);
    if (v == 0) { pthread_mutex_lock(&tb_slock); free(tb_snds[s].d); tb_snds[s].d = 0; pthread_mutex_unlock(&tb_slock); }
    return v;
}
TB_API void tb_stop_sound(long long id) {
    pthread_mutex_lock(&tb_slock);
    tb_voice *v = tb_voice_at(id);
    if (v) v->stop = 1;   // микшер плавно уведёт в тишину за 10 мс и освободит голос
    pthread_mutex_unlock(&tb_slock);
}
TB_API _Bool tb_sound_playing(long long id) {
    pthread_mutex_lock(&tb_slock);
    _Bool r = tb_voice_at(id) != 0;
    pthread_mutex_unlock(&tb_slock);
    return r;
}
TB_API void tb_sound_volume(long long id, double k) {
    if (k < 0) k = 0;
    if (k > 4) k = 4;
    pthread_mutex_lock(&tb_slock);
    tb_voice *v = tb_voice_at(id);
    if (v) v->vol = k;
    pthread_mutex_unlock(&tb_slock);
}
TB_API void tb_sound_pitch(long long id, double k) {
    if (k < 0.05) k = 0.05;
    if (k > 8) k = 8;
    pthread_mutex_lock(&tb_slock);
    tb_voice *v = tb_voice_at(id);
    if (v) v->pitch = k;
    pthread_mutex_unlock(&tb_slock);
}
TB_API void tb_move_sound(long long id, double x, double y, double z) {
    pthread_mutex_lock(&tb_slock);
    tb_voice *v = tb_voice_at(id);
    if (v) { v->x = x; v->y = y; v->z = z; }
    pthread_mutex_unlock(&tb_slock);
}
TB_API void tb_master_volume(double k) {
    if (k < 0) k = 0;
    if (k > 4) k = 4;
    pthread_mutex_lock(&tb_slock);
    tb_master = k;
    pthread_mutex_unlock(&tb_slock);
}
// слушатель: где уши и куда смотрит (как camera; вверх — ось y)
TB_API void tb_listener(double x, double y, double z, double lx, double ly, double lz) {
    double f[3] = { lx - x, ly - y, lz - z }, l = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (l < 1e-12) return;
    for (int i = 0; i < 3; i++) f[i] /= l;
    double up[3] = { 0, 1, 0 };
    if (fabs(f[1]) > 0.9999) { up[1] = 0; up[2] = 1; }
    double r[3] = { up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0] };
    l = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    pthread_mutex_lock(&tb_slock);
    tb_lis[0] = x; tb_lis[1] = y; tb_lis[2] = z;
    for (int i = 0; i < 3; i++) { tb_lis[3 + i] = r[i] / l; tb_lis[6 + i] = f[i]; }
    pthread_mutex_unlock(&tb_slock);
}
#endif
#if defined(TB_LL)
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
TB_X(const char *, ctext, (const char *p), (p))
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
TB_X(_Bool, chan_ready, (tb_chan *ch), (ch))
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
#ifndef TB_BARE
TB_XV(call_main, (void (*f)(void)), (f))
#endif
TB_XV(task_start, (tb_task *t, void *(*run)(void *), void *arg), (t, run, arg))
TB_X(void *, wait, (tb_task *t), (t))
TB_X(_Bool, crashed, (tb_task *t), (t))
void tb_x_show_i(long long x) { printf("%lld", x); }
void tb_x_show_d(double x) { printf("%g", x); }
#ifdef TB_BARE
// ядро: начало tb_kmain из .ll и прерывания
void tb_x_kstart(void) { tb_args = tb_list_new(sizeof(const char *)); }
void tb_x_irq_set(int v, void (*fn)(void)) { tb_irq_set(v, fn); }
#else
void tb_x_start(int argc, char **argv, void *top) {
    tb_guard_init();
    tb_gc_init(top);
    tb_gc_root((void *)&tb_args, sizeof tb_args);
    tb_gc_root((void *)&tb_err, sizeof tb_err);
    tb_args = tb_list_new(sizeof(const char *));
    for (int i = 1; i < argc; i++) *(const char **)tb_grow(tb_args) = argv[i];
}
#endif
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
#ifndef TB_BARE
TB_B(tb_list *, list_dir, (const char *s, int line), tb_list_dir(s))
#endif
#ifndef TB_BARE
TB_B(_Bool, is_folder, (const char *s, int line), tb_is_folder(s))
#endif
#ifndef TB_BARE
TB_BV(make_folder, (const char *s, int line), tb_make_folder(s))
#endif
#ifndef TB_BARE
TB_BV(move, (const char *a, const char *b, int line), tb_move(a, b))
#endif
#ifndef TB_BARE
TB_BV(remove, (const char *s, int line), tb_remove(s))
#endif
TB_B(const char *, env, (const char *s, int line), tb_env(s))
#ifndef TB_BARE
TB_B(long long, file_size, (const char *s, int line), tb_file_size(s))
#endif
TB_B(long long, rgb, (long long r, long long g, long long b, int line), tb_rgb(r, g, b))
TB_B(long long, rgba, (long long r, long long g, long long b, long long a, int line), tb_rgba(r, g, b, a))
TB_B(long long, random, (long long a, long long b, int line), tb_random(a, b))
TB_B(long long, bit_and, (long long a, long long b, int line), tb_band(a, b))
TB_B(long long, bit_or, (long long a, long long b, int line), tb_bor(a, b))
TB_B(long long, bit_xor, (long long a, long long b, int line), tb_bxor(a, b))
TB_B(long long, bit_not, (long long a, int line), tb_bnot(a))
TB_B(long long, shift_left, (long long a, long long b, int line), tb_shl(a, b))
TB_B(long long, shift_right, (long long a, long long b, int line), tb_shr(a, b))
TB_BV(wait_ms, (long long n, int line), tb_wait_ms(n))
#ifndef TB_BARE
TB_B(const char *, http_get, (const char *s, int line), tb_http_get(s))
#endif
#ifndef TB_BARE
TB_B(const char *, json_get, (const char *s, const char *p, int line), tb_json_get(s, p))
#endif
#ifndef TB_BARE
TB_B(const char *, json_text, (const char *s, int line), tb_json_text(s))
#endif
#ifndef TB_BARE
TB_B(const char *, date_now, (int line), tb_date_now())
#endif
#ifndef TB_BARE
TB_B(long long, unix_time, (int line), tb_unix_time())
#endif
TB_B(long long, time_ms, (int line), tb_time_ms())
TB_B(const char *, input, (int line), tb_input())
TB_B(tb_list *, args, (int line), tb_args)
TB_BV(sort_int, (tb_list *l, int line), tb_sort_list(l, tb_cmp_int))
TB_BV(sort_dec, (tb_list *l, int line), tb_sort_list(l, tb_cmp_dec))
TB_BV(sort_text, (tb_list *l, int line), tb_sort_list(l, tb_cmp_text))
TB_B(double, sqrt, (double x, int line), tb_sqrt(x, line))
TB_B(long long, read8, (long long a, int line), tb_rd8(a, line))
TB_BV(write8, (long long a, long long v, int line), tb_wr8(a, v, line))
TB_B(long long, read16, (long long a, int line), tb_rd16(a, line))
TB_BV(write16, (long long a, long long v, int line), tb_wr16(a, v, line))
TB_B(long long, read32, (long long a, int line), tb_rd32(a, line))
TB_BV(write32, (long long a, long long v, int line), tb_wr32(a, v, line))
TB_B(long long, read64, (long long a, int line), tb_rd64(a, line))
TB_BV(write64, (long long a, long long v, int line), tb_wr64(a, v, line))
TB_B(long long, address_of, (tb_list *l, int line), tb_addr_of(l))
TB_B(long long, new_mem, (long long n, int line), tb_new_mem(n, line))
TB_BV(free_mem, (long long a, int line), tb_free_mem(a, line))
TB_B(long long, grow_mem, (long long a, long long n, int line), tb_grow_mem(a, n, line))
TB_BV(copy_mem, (long long d, long long s, long long n, int line), tb_copy_mem(d, s, n, line))
TB_BV(fill_mem, (long long a, long long v, long long n, int line), tb_fill_mem(a, v, n, line))
TB_B(long long, shape_get, (long long a, long long k, int line), tb_shape_get(a, k, line))
TB_B(long long, zone_new, (long long n, int line), tb_zone_new(n, line))
TB_B(long long, zone_take, (long long z, long long n, int line), tb_zone_take(z, n, line))
TB_BV(zone_clear, (long long z, int line), tb_zone_clear(z, line))
TB_BV(zone_free, (long long z, int line), tb_zone_free(z, line))
TB_B(long long, zone_used, (long long z, int line), tb_zone_used(z, line))
#ifndef TB_BARE
TB_B(long long, syscall, (long long n, long long a, long long b, long long c, long long d, long long e, long long f, int line), tb_syscall(n, a, b, c, d, e, f, line))
#endif
TB_BV(shape_set, (long long a, long long k, long long x, int line), tb_shape_set(a, k, x, line))
TB_B(long long, shape_at, (long long a, int line), tb_shape_at(a))
TB_B(long long, abs_i, (long long x, int line), tb_absi(x))
TB_B(double, abs_d, (double x, int line), fabs(x))
TB_B(long long, min_i, (long long a, long long b, int line), tb_mini(a, b))
TB_B(long long, max_i, (long long a, long long b, int line), tb_maxi(a, b))
TB_B(double, min_d, (double a, double b, int line), tb_mind(a, b))
TB_B(double, max_d, (double a, double b, int line), tb_maxd(a, b))
#ifdef TB_BARE
TB_B(long long, port_in8, (long long p, int line), tb_pin8(p))
TB_B(long long, port_in16, (long long p, int line), tb_pin16(p))
TB_B(long long, port_in32, (long long p, int line), tb_pin32(p))
TB_BV(port_out8, (long long p, long long v, int line), tb_pout8(p, v))
TB_BV(port_out16, (long long p, long long v, int line), tb_pout16(p, v))
TB_BV(port_out32, (long long p, long long v, int line), tb_pout32(p, v))
TB_B(long long, page_new, (int line), tb_page_new())
TB_BV(page_free, (long long a, int line), tb_page_free(a))
TB_BV(interrupts_on, (int line), tb_irq_on())
TB_BV(interrupts_off, (int line), tb_irq_off())
TB_B(long long, uptime_ms, (int line), tb_uptime_ms())
TB_B(const char *, read_key, (int line), tb_read_key())
TB_BV(yield, (int line), tb_yield())
#endif
#ifdef TB_SND
TB_B(long long, load_sound, (const char *s, int line), tb_load_sound(s))
TB_B(long long, play_sound, (long long s, int line), tb_play_sound(s))
TB_B(long long, loop_sound, (long long s, int line), tb_loop_sound(s))
TB_B(long long, play_sound_at, (long long s, double x, double y, double z, int line), tb_play_sound_at(s, x, y, z))
TB_B(long long, loop_sound_at, (long long s, double x, double y, double z, int line), tb_loop_sound_at(s, x, y, z))
TB_B(long long, play_tone, (double hz, long long ms, int line), tb_play_tone(hz, ms))
TB_BV(stop_sound, (long long v, int line), tb_stop_sound(v))
TB_B(_Bool, sound_playing, (long long v, int line), tb_sound_playing(v))
TB_BV(sound_volume, (long long v, double k, int line), tb_sound_volume(v, k))
TB_BV(sound_pitch, (long long v, double k, int line), tb_sound_pitch(v, k))
TB_BV(move_sound, (long long v, double x, double y, double z, int line), tb_move_sound(v, x, y, z))
TB_BV(master_volume, (double k, int line), tb_master_volume(k))
TB_BV(listener, (double x, double y, double z, double lx, double ly, double lz, int line), tb_listener(x, y, z, lx, ly, lz))
#endif
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
TB_B(long long, mouse_wheel, (int line), tb_mouse_wheel())
TB_B(_Bool, mouse_back, (int line), tb_mouse_back())
TB_B(_Bool, mouse_forward, (int line), tb_mouse_forward())
TB_B(const char *, typed_text, (int line), tb_typed_text())
TB_B(const char *, clipboard_get, (int line), tb_clipboard_get())
TB_BV(clipboard_set, (const char *s, int line), tb_clipboard_set(s))
TB_BV(set_cursor, (const char *s, int line), tb_set_cursor(s, line))
TB_BV(window_sharp, (int line), tb_window_sharp())
TB_B(double, window_scale, (int line), tb_window_scale())
TB_B(double, frame_ms, (int line), tb_frame_ms())
TB_B(long long, fps, (int line), tb_fps())
TB_BV(set_fps, (long long n, int line), tb_set_fps(n))
TB_B(long long, window_w, (int line), tb_window_w())
TB_B(long long, window_h, (int line), tb_window_h())
TB_B(long long, load_image, (const char *s, int line), tb_load_image(s))
TB_BV(draw_image, (long long i, long long x, long long y, int line), tb_draw_image(i, x, y))
TB_B(long long, mesh_new, (int line), tb_mesh_new())
TB_BV(mesh_add, (long long m, double x1, double y1, double z1, double x2, double y2, double z2, double x3, double y3, double z3, long long c, int line), tb_mesh_add(m, x1, y1, z1, x2, y2, z2, x3, y3, z3, c))
TB_BV(draw_mesh, (long long m, double x, double y, double z, double t, int line), tb_draw_mesh(m, x, y, z, t))
TB_BV(camera, (double x, double y, double z, double lx, double ly, double lz, int line), tb_camera(x, y, z, lx, ly, lz))
TB_BV(draw_triangle, (long long x1, long long y1, long long x2, long long y2, long long x3, long long y3, long long c, int line), tb_draw_triangle(x1, y1, x2, y2, x3, y3, c))
TB_B(long long, image_w, (long long i, int line), tb_image_w(i))
TB_B(long long, screen_image, (int line), tb_screen_image())
TB_B(long long, image_h, (long long i, int line), tb_image_h(i))
TB_B(long long, load_font, (const char *s, int line), tb_load_font(s))
TB_BV(draw_font, (long long f, long long x, long long y, const char *s, long long size, long long c, int line), tb_draw_font(f, x, y, s, size, c))
TB_B(long long, text_width, (long long f, const char *s, long long size, int line), tb_text_width(f, s, size))
#endif
#endif
