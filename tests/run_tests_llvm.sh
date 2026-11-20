#!/bin/bash
# То же, что run_tests.sh, но каждая программа собирается с --llvm (числовые функции через LLVM IR).
# Проверка компилятора.
#   tests/ok/*.tb  — программа должна собраться, и её вывод совпасть с .out
#   tests/err/*.tb — компилятор (или программа) должен выдать ошибку из .err
#   tests/mod/*/   — программа из нескольких файлов (main.tb + модули), эталон в expected
# Запуск из корня репозитория:  ./tests/run_tests.sh
# Обновить эталоны после намеренной правки:  ./tests/run_tests.sh --update
# Тесты гоняются через компилятор на Tachbit (он собирается из семени, без стартера на C).
cd "$(dirname "$0")/.." || exit 1
export LC_ALL=C.UTF-8 2>/dev/null
compiler="build/tb"
# без стартера: семя compiler/seed/tb.c → компилятор из compiler/*.tb → он же сам собой
./tools/build_tb.sh > /dev/null || exit 1

pass=0
bad=0
tmp="$(mktemp -d)"

for f in tests/ok/*.tb tests/err/*.tb; do
    name="$(basename "$f" .tb)"
    cp "$f" "$tmp/$name.tb"
    chk=; case "$name" in memcheck_*) chk=1 ;; esac
    got="$( (cd "$tmp" && TB_MEM_CHECK=$chk "$OLDPWD/$compiler" run --llvm "$name.tb") 2>&1)"
    case "$f" in
        tests/ok/*) want_file="${f%.tb}.out" ;;
        *) want_file="${f%.tb}.err" ;;
    esac
    if [ "$1" = "--update" ]; then
        printf '%s\n' "$got" > "$want_file"
    fi
    if [ "$got" = "$(cat "$want_file" 2>/dev/null)" ]; then
        pass=$((pass + 1))
    else
        bad=$((bad + 1))
        echo "НЕ СОВПАЛО: $f"
        echo "  ждали:    $(cat "$want_file" 2>/dev/null | head -3)"
        echo "  получили: $(echo "$got" | head -3)"
    fi
done
# tests/mod/*/main.tb — программы из нескольких файлов; эталон в expected
for d in tests/mod/*/; do
    name="$(basename "$d")"
    mkdir -p "$tmp/mod/$name"
    cp "$d"*.tb "$tmp/mod/$name/"
    got="$( (cd "$tmp/mod/$name" && "$OLDPWD/$compiler" run --llvm main.tb) 2>&1)"
    if [ "$1" = "--update" ]; then
        printf '%s\n' "$got" > "${d}expected"
    fi
    if [ "$got" = "$(cat "${d}expected" 2>/dev/null)" ]; then
        pass=$((pass + 1))
    else
        bad=$((bad + 1))
        echo "НЕ СОВПАЛО: $d"
        echo "  ждали:    $(cat "${d}expected" 2>/dev/null | head -3)"
        echo "  получили: $(echo "$got" | head -3)"
    fi
done
rm -rf "$tmp"
echo "прошло: $pass, не прошло: $bad"
[ "$bad" = "0" ]
