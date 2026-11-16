#!/bin/bash
# Проверка компилятора.
#   tests/ok/*.tb  — программа должна собраться, и её вывод совпасть с .out
#   tests/err/*.tb — компилятор (или программа) должен выдать ошибку из .err
#   tests/mod/*/   — программа из нескольких файлов (main.tb + модули), эталон в expected
# Запуск из корня репозитория:  ./tests/run_tests.sh
# Обновить эталоны после намеренной правки:  ./tests/run_tests.sh --update
# Тесты гоняются через компилятор на Tachbit (сначала он собирает сам себя).
# Только стартер на C (умеет всё до 0.5):  ./tests/run_tests.sh --c
cd "$(dirname "$0")/.." || exit 1
cc -std=c99 -Wall -Wextra -o tachbit compiler-c/*.c || exit 1
export LC_ALL=C.UTF-8 2>/dev/null
compiler="tachbit-tb"
if [ "$1" = "--c" ]; then
    compiler="tachbit"
    echo "проверяю стартер на C (тесты до 0.5)"
else
    # раскрутка: стартер на C собирает компилятор на Tachbit, тот собирает себя сам
    ./tachbit build compiler-tb/main.tb || exit 1
    mv compiler-tb/main tachbit-tb2
    ./tachbit-tb2 build compiler-tb/main.tb || exit 1
    mv compiler-tb/main tachbit-tb
    rm -f tachbit-tb2 compiler-tb/main.c
fi

pass=0
bad=0
tmp="$(mktemp -d)"

for f in tests/ok/*.tb tests/err/*.tb; do
    name="$(basename "$f" .tb)"
    if [ "$compiler" = "tachbit" ] && head -1 "$f" | grep -q "0.7"; then
        continue   # новое в 0.7 умеет только компилятор на Tachbit
    fi
    cp "$f" "$tmp/$name.tb"
    got="$( (cd "$tmp" && "$OLDPWD/$compiler" run "$name.tb") 2>&1)"
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
    got="$( (cd "$tmp/mod/$name" && "$OLDPWD/$compiler" run main.tb) 2>&1)"
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
