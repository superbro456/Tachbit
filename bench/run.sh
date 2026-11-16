#!/bin/bash
# Замер скорости: одна и та же задача на Tachbit, C и Python.
# Запуск из корня репозитория:  ./bench/run.sh
cd "$(dirname "$0")/.." || exit 1
cc -std=c99 -O2 -o tachbit compiler-c/*.c || exit 1
cd bench || exit 1

# время запуска в секундах: встроенный в bash time, одинаково на Mac и Linux
seconds() {
    local TIMEFORMAT=%R
    { time "$@" > /dev/null 2>&1 ; } 2>&1
}

# каждую программу запускаем 3 раза и берём лучшее время (первый запуск бывает медленным)
best() {
    local best_time="" t
    for run in 1 2 3; do
        t=$(seconds "$@")
        if [ -z "$best_time" ] || awk "BEGIN { exit !($t < $best_time) }"; then
            best_time=$t
        fi
    done
    echo "$best_time"
}

for task in fib primes; do
    ../tachbit build $task.tb > /dev/null || exit 1
    cc -O2 -o ${task}_c ${task}_pure.c || exit 1
    echo "== $task  (ответ: $(./$task))"
    echo "   Tachbit: $(best ./$task) с"
    echo "   C:       $(best ./${task}_c) с"
    if command -v python3 > /dev/null; then
        echo "   Python:  $(seconds python3 $task.py) с   (Python медленный: ждём один запуск)"
    fi
    rm -f $task $task.c ${task}_c
done
