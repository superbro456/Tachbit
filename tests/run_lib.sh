#!/bin/bash
# Проверка библиотек из lib/ (physics, anim ...): каждая tests/lib/*.tb печатает то же, что в .out,
# и по пути LLVM, и по пути C.
export TB_LANG=ru   # эталоны сообщений — на русском
cd "$(dirname "$0")/.." || exit 1
[ -x ./build/tb ] || ./tools/build_tb.sh > /dev/null || exit 1
export TB_LIB="$PWD/lib"
ok=0; bad=0
for f in tests/lib/*.tb; do
    for mode in "" "--c"; do
        out=$(./build/tb run $mode "$f" 2>&1)
        if [ "$out" = "$(cat "${f%.tb}.out")" ]; then ok=$((ok + 1)); else echo "НЕ СОВПАЛО: $f $mode"; diff <(echo "$out") "${f%.tb}.out" | head -10; bad=$((bad + 1)); fi
    done
done
echo "библиотеки: $ok ок, $bad с ошибками"
[ $bad = 0 ]
