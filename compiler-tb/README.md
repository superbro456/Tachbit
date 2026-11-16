# compiler-tb — компилятор Tachbit, написанный на Tachbit (версия 0.5)

Здесь появится тот же компилятор, но на языке Tachbit.

Как это будет работать (раскрутка, bootstrap):

1. `compiler-c/` (компилятор на C) собирается один раз командой `cc` → программа `tachbit`.
2. `./tachbit build compiler-tb/main.tb` → `tachbit2` (компилятор Tachbit, собранный из Tachbit).
3. `./tachbit2 build compiler-tb/main.tb` → `tachbit3`. Если `tachbit2` и `tachbit3` выдают одинаковый C — язык сам себя собирает.

После этого компилятор на C нужен только как «стартер» на новом компьютере.
