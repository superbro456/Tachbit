#!/bin/bash
# Проверка компилятора.
#   tests/ok/*.tb  — программа должна собраться, и её вывод совпасть с .out
#   tests/err/*.tb — компилятор (или программа) должен выдать ошибку из .err
# Запуск из корня репозитория:  ./tests/run_tests.sh
# Обновить эталоны после намеренной правки:  ./tests/run_tests.sh --update
cd "$(dirname "$0")/.." || exit 1
cc -std=c99 -Wall -Wextra -o tachbit src/*.c || exit 1

pass=0
bad=0
tmp="$(mktemp -d)"

for f in tests/ok/*.tb tests/err/*.tb; do
    name="$(basename "$f" .tb)"
    cp "$f" "$tmp/$name.tb"
    got="$( (cd "$tmp" && "$OLDPWD/tachbit" run "$name.tb") 2>&1)"
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
rm -rf "$tmp"
echo "прошло: $pass, не прошло: $bad"
[ "$bad" = "0" ]
