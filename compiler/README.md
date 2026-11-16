# compiler — компилятор Tachbit, написанный на Tachbit (0.5)

Тот же компилятор, что в `archive/compiler-c/`, строчка в строчку, но на Tachbit.
Вместо глобальных переменных — один объект `Comp`, который передаётся во все функции.

Раскрутка (bootstrap):

1. `cc -std=c99 -O2 -o tachbit archive/compiler-c/*.c` — стартер на C.
2. `./tachbit build compiler/main.tb` → `compiler/main` (компилятор на Tachbit). Переименуй его в `tachbit2`.
3. `./tachbit2 build compiler/main.tb` → снова `compiler/main`: компилятор собрал сам себя.

Проверка: `./tests/run_tests.sh` делает эти шаги и гоняет все тесты через компилятор на Tachbit.

`runtime_text.tb` не правь руками: он делается из `compiler/runtime.h` командой `python3 tools/embed_runtime.py`.
