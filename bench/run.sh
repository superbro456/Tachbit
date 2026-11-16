#!/bin/bash
# Замер скорости: одна и та же задача на Tachbit, C и Python.
# Запуск из корня репозитория:  ./bench/run.sh
cd "$(dirname "$0")/.." || exit 1
cc -std=c99 -O2 -o tachbit src/*.c || exit 1
cd bench || exit 1

seconds() {
    local start end
    start=$(date +%s.%N 2>/dev/null)
    if [ "$start" = "" ] || [[ "$start" == *N ]]; then
        start=$(python3 -c 'import time; print(time.time())')
        "$@" > /dev/null
        end=$(python3 -c 'import time; print(time.time())')
    else
        "$@" > /dev/null
        end=$(date +%s.%N)
    fi
    echo "$start $end" | awk '{ printf "%.2f", $2 - $1 }'
}

for task in fib primes; do
    ../tachbit build $task.tb > /dev/null || exit 1
    cc -O2 -o ${task}_c ${task}_pure.c || exit 1
    echo "== $task  (ответ: $(./$task))"
    echo "   Tachbit: $(seconds ./$task) с"
    echo "   C:       $(seconds ./${task}_c) с"
    if command -v python3 > /dev/null; then
        echo "   Python:  $(seconds python3 $task.py) с"
    fi
    rm -f $task $task.c ${task}_c
done
