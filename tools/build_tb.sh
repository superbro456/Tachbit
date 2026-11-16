#!/bin/bash
# Собирает компилятор Tachbit без стартера на C.
#   1) compiler-tb/seed/tb.c — готовый C-код компилятора (его сделал сам Tachbit);
#      cc превращает его в программу «семя»;
#   2) семя собирает свежий компилятор из compiler-tb/*.tb;
#   3) свежий собирает себя ещё раз — это и есть ./tachbit-tb.
# Нужен только cc (clang на Mac, gcc на Linux): Tachbit пишет C, а cc делает из него программу.
cd "$(dirname "$0")/.." || exit 1
cc=${CC:-cc}
$cc -O2 -w -o ./tachbit-seed compiler-tb/seed/tb.c -lm -lpthread || { echo "не получилось собрать семя"; exit 1; }
./tachbit-seed build compiler-tb/main.tb > /dev/null || exit 1
mv compiler-tb/main ./tachbit-tb2
./tachbit-tb2 build compiler-tb/main.tb > /dev/null || exit 1
mv compiler-tb/main ./tachbit-tb
rm -f ./tachbit-seed ./tachbit-tb2 compiler-tb/main.c
echo "собран ./tachbit-tb"
