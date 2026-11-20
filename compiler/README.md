# compiler — компилятор Tachbit, написанный на Tachbit (0.5)

Компилятор Tachbit, написанный на самом Tachbit (первый стартер на C убран, он есть только в истории git).
Вместо глобальных переменных — один объект `Comp`, который передаётся во все функции.

Раскрутка (bootstrap):

1. `cc -O2 -o build/tb-seed compiler/seed/tb.c -lm -lpthread` — семя (C-код этого же компилятора).
2. `./build/tb-seed build compiler/main.tb` → `compiler/main` (свежий компилятор). Он переезжает в `build/tb2`.
3. `./build/tb2 build compiler/main.tb` → снова `compiler/main`: компилятор собрал сам себя, это `build/tb`.

Всё это делает `./tools/build_tb.sh`.

Проверка: `./tests/run_tests.sh` делает эти шаги и гоняет все тесты через компилятор на Tachbit.

`runtime_text.tb` не правь руками: он делается из `compiler/runtime.h` командой `python3 tools/embed_runtime.py`.
