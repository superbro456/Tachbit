#!/bin/bash
# Tachbit ↔ C: tb lib делает libmylib.a и mylib.h, программа на C зовёт функции for c.
export TB_LANG=ru   # эталоны сообщений — на русском
cd "$(dirname "$0")/.." || exit 1
[ -x ./build/tb ] || ./tools/build_tb.sh > /dev/null || exit 1
tmp=$(mktemp -d)
cp tests/clib/mylib.tb "$tmp/"
ok=0; bad=0
if ./build/tb lib "$tmp/mylib.tb" > /dev/null && ${CC:-cc} -o "$tmp/use" tests/clib/use_mylib.c -I"$tmp" "$tmp/libmylib.a" -lm -lpthread; then
    out=$("$tmp/use" 2>&1)
    if [ "$out" = "$(cat tests/clib/use_mylib.out)" ]; then ok=1; else echo "НЕ СОВПАЛО: use_mylib"; diff <(echo "$out") tests/clib/use_mylib.out; bad=1; fi
else
    echo "не собралось: tests/clib"; bad=1
fi
rm -rf "$tmp"
echo "C ↔ Tachbit: $ok ок, $bad с ошибками"
[ $bad = 0 ]
