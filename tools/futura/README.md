# Tachbit для FuturaOS

FuturaOS — ОС Алексея (x86-64, выросла из ToaruOS). Здесь лежит всё, чтобы программы
на Tachbit запускались внутри неё. Сам язык и компилятор не менялись: компилятор делает
обычный C (`tb c`), а этот C собирается под libc FuturaOS.

## Что нужно

- `tb` (Tachbit 1.3), `clang` и `ld.lld` (на Linux: `apt install clang lld`);
- `python3` с модулем `lz4` (`python3 -m pip install lz4`) — только чтобы достать файлы из ramdisk;
- `ramdisk.igz` из образа FuturaOS.

## Как собрать программу

```sh
# 1. один раз: достать libc.so, libm.so, crt0.o, crti.o, crtn.o из его ramdisk
python3 tools/futura/ramdisk.py sysroot ramdisk.igz ~/.tachbit/futura

# 2. собрать программу
tools/futura/build.sh hello.tb            # получится hello.futura
```

Готовый файл кладут в FuturaOS, например в `/bin`, и запускают как обычную программу.
Чтобы проверить у себя, можно вшить программу в ramdisk:

```sh
python3 tools/futura/ramdisk.py unpack ramdisk.igz ramdisk.tar
python3 tools/futura/ramdisk.py add ramdisk.tar new.igz bin/hello=hello.futura
```

`add` печатает новые `rd_isize=… rd_csize=…`: их надо поставить в строку загрузки ядра
вместо старых.

## Как это устроено

- `include/tb_futura.h` подключается раньше всего (`clang -include`) и закрывает
  разницу между заголовками ToaruOS и libc FuturaOS.
- `toaru-include/` — заголовки ToaruOS (лицензия NCSA, файл `LICENSE-ToaruOS`).
  Своих заголовков libc в образе FuturaOS нет, а libc у него сильно своя.
- Программа собирается как у его программ: обычный ELF с адреса `0x400000`,
  `libc.so` подключается через `/lib/ld.so`.

### Что пришлось обойти (это стоит рассказать Алексею)

1. **Замки.** `pthread_mutex_t` и `pthread_cond_t` в его libc устроены не так, как в ToaruOS
   (свои «compact mutex»). Без его заголовков их размер неизвестен, поэтому Tachbit использует
   свои замки на атомиках + `sched_yield`. Потоки создаёт его `pthread_create`.
2. **TLS в статических программах.** В статически собранной программе его libc не готовит
   место под `_Thread_local` (`__futura_tls_set_static_layout` никто не зовёт), и программа
   падает на первом обращении. Поэтому сборка динамическая, через `ld.so`.
3. **NX в `mmap`.** Страницы из `mmap` без `PROT_EXEC` получают бит NX, а запись в них падает
   с ошибкой «reserved bit» (`err=0xe`). Похоже, ядро не включает `EFER.NXE`.
   Обход: Tachbit просит страницы с `PROT_EXEC`.
4. **`printf("%g", 0.0)`** печатает `0e-1074`. Обход: Tachbit поправляет это в своём выводе.
5. `errno` — это `__errno_location()` (в ToaruOS было `__errno_addr()`).
6. `syscall()` нет, номера вызовов свои: в Tachbit `syscall(...)` отдаёт ошибку ENOSYS.
7. В `esh` строка `prog </dev/null >файл 2>файл` передаёт `</dev/null` программе как слово запуска,
   а `2>&1` и `&` в конце строки не работают ("Read only file system").

## Что проверено

Ядро `KERN.BIN` и ramdisk из образа FuturaOS, загрузка через GRUB (multiboot2) в QEMU
(q35, 2 ядра, 4 ГБ, UEFI). Тесты `tests/ok` (76 программ) собраны и запущены внутри FuturaOS.
65 из 76 совпали с эталоном буква в букву. Остальные отличаются только тем, что зависит от окружения:

- сообщения об ошибках по-английски: в FuturaOS не задан `LANG`/`TB_LANG`
  (`TB_LANG=ru` или файл `~/.tachbit/lang` с `ru` включает русский);
- сеть через `localhost` не работает (connection refused): в QEMU у его ОС нет сети;
- `env("HOME")` пустой у программ, которые запускает init;
- `syscall(39)` — номер вызова Linux, в FuturaOS его нет.

Не проверено: окна и рисование (Tachbit рисует через X11/Cocoa/Win32, у FuturaOS свой
композитор), `use c` с файлами C.
