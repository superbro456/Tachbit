#!/bin/bash
# Замер скорости: одна и та же задача на Tachbit, C и Python.
# Запуск из корня репозитория:  ./bench/run.sh
cd "$(dirname "$0")/.." || exit 1
# компилятор на Tachbit: стартер на C собирает его, потом он собирает себя сам
cc -std=c99 -O2 -o tachbit compiler-c/*.c || exit 1
./tachbit build compiler-tb/main.tb > /dev/null || exit 1
mv compiler-tb/main tachbit-tb2
./tachbit-tb2 build compiler-tb/main.tb > /dev/null || exit 1
mv compiler-tb/main tachbit-tb
rm -f tachbit-tb2 compiler-tb/main.c
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

for task in fib primes cores; do
    ../tachbit-tb build $task.tb > /dev/null || exit 1
    cc -O2 -o ${task}_c ${task}_pure.c || exit 1
    echo "== $task  (ответ: $(./$task))"
    echo "   Tachbit: $(best ./$task) с"
    echo "   C:       $(best ./${task}_c) с"
    if command -v python3 > /dev/null; then
        echo "   Python:  $(seconds python3 $task.py) с   (Python медленный: ждём один запуск)"
    fi
    rm -f $task $task.c ${task}_c
done

# by columns: сумма одного качества у 20 млн объектов (10 раз)
../tachbit-tb build columns.tb > /dev/null || exit 1
../tachbit-tb build cards.tb > /dev/null || exit 1
cc -O2 -o columns_c columns_pure.c || exit 1
echo "== columns  (ответ: $(./columns))"
echo "   Tachbit by columns:   $(best ./columns) с"
echo "   Tachbit обычный obj:  $(best ./cards) с"
echo "   C (массив структур):  $(best ./columns_c) с"
if command -v python3 > /dev/null; then
    py=$(seconds python3 columns.py)
    echo "   Python:  $(awk "BEGIN { print $py * 10 }") с   (считал 2 млн объектов, время ×10)"
fi
rm -f columns columns.c cards cards.c columns_c

# temp: 100 раз собрать 100 тысяч строк; время и сколько памяти заняла программа
mem() {
    if command -v python3 > /dev/null; then
        python3 -c "import resource, subprocess, sys
subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL)
r = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
print(r // (1024 * 1024) if sys.platform == 'darwin' else r // 1024)" "$@"
    else
        echo "?"
    fi
}
../tachbit-tb build temp.tb > /dev/null || exit 1
../tachbit-tb build notemp.tb > /dev/null || exit 1
cc -O2 -o temp_c temp_pure.c || exit 1
echo "== temp  (ответ: $(./temp))"
echo "   Tachbit с temp:   $(best ./temp) с, память $(mem ./temp) МБ"
echo "   Tachbit без temp: $(best ./notemp) с, память $(mem ./notemp) МБ"
echo "   C (malloc/free):  $(best ./temp_c) с, память $(mem ./temp_c) МБ"
if command -v python3 > /dev/null; then
    echo "   Python:  $(seconds python3 temp.py) с"
fi
rm -f temp temp.c notemp notemp.c temp_c

# by vec: сумма квадратов 10 млн дробей, 20 раз
../tachbit-tb build vec.tb > /dev/null || exit 1
../tachbit-tb build novec.tb > /dev/null || exit 1
cc -O2 -o vec_c vec_pure.c || exit 1
echo "== vec  (ответ: $(./vec))"
echo "   Tachbit by vec:    $(best ./vec) с"
echo "   Tachbit без vec:   $(best ./novec) с"
echo "   C (обычный цикл):  $(best ./vec_c) с"
if command -v python3 > /dev/null; then
    py=$(seconds python3 vec.py)
    echo "   Python:  $(awk "BEGIN { print $py * 10 }") с   (считал 1 млн чисел, время ×10)"
fi
rm -f vec vec.c novec novec.c vec_c

# by N: 4 прохода по 20 млн дробей (160 МБ), 5 раз
../tachbit-tb build pieces.tb > /dev/null || exit 1
../tachbit-tb build nopieces.tb > /dev/null || exit 1
cc -O2 -o pieces_c pieces_pure.c || exit 1
echo "== pieces  (ответ: $(./pieces))"
echo "   Tachbit by 4096:     $(best ./pieces) с"
echo "   Tachbit без кусков:  $(best ./nopieces) с"
echo "   C (проходы подряд):  $(best ./pieces_c) с"
rm -f pieces pieces.c nopieces nopieces.c pieces_c

# together: 16 млн поисков в dict на 4 млн ключей, по 8 за раз
../tachbit-tb build together.tb > /dev/null || exit 1
../tachbit-tb build notogether.tb > /dev/null || exit 1
cc -O2 -o together_c together_pure.c || exit 1
echo "== together  (ответ: $(./together))"
echo "   Tachbit together:     $(best ./together) с"
echo "   Tachbit без together: $(best ./notogether) с"
echo "   C (та же таблица):    $(best ./together_c) с"
rm -f together together.c notogether notogether.c together_c
