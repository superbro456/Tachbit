#!/bin/bash
# Путь через C против пути через LLVM IR (--llvm) на числовых задачах.
# Оба пути собирает clang, чтобы сравнивать вывод Tachbit, а не два разных компилятора C.
# Запуск из корня репозитория:  ./bench/llvm/run.sh
cd "$(dirname "$0")/../.." || exit 1
./tools/build_tb.sh > /dev/null || exit 1
T="$PWD/tachbit-tb"
cd bench/llvm || exit 1
seconds() { local TIMEFORMAT=%R; { time "$@" > /dev/null 2>&1 ; } 2>&1; }
best() {
    local b="" t
    for r in 1 2 3; do
        t=$(seconds "$@")
        if [ -z "$b" ] || awk "BEGIN { exit !($t < $b) }"; then b=$t; fi
    done
    echo "$b"
}
printf "%-12s %8s %8s  %s\n" "задача" "через C" "LLVM" "функций в .ll"
for b in fib collatz mandel primes_div gcd hist axpy prefix; do
    src=$b.tb
    [ "$b" = fib ] && src=../fib.tb
    CC=clang "$T" build "$src" > /dev/null || exit 1
    mv "${src%.tb}" "/tmp/tb_ll_${b}_c"
    "$T" build --llvm "$src" > /dev/null || exit 1
    mv "${src%.tb}" "/tmp/tb_ll_${b}_ll"
    rm -f "${src%.tb}.c" "${src%.tb}.ll"
    n=$("$T" ll "$src" | grep -c '^define')
    printf "%-12s %7ss %7ss  %s\n" "$b" "$(best /tmp/tb_ll_${b}_c)" "$(best /tmp/tb_ll_${b}_ll)" "$n"
    rm -f "/tmp/tb_ll_${b}_c" "/tmp/tb_ll_${b}_ll"
done
