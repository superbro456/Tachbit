#!/bin/bash
# Замер скорости: одна и та же задача на Tachbit, C и Python.
# Запуск из корня репозитория:  ./bench/run.sh   (без Python: ./bench/run.sh --fast)
cd "$(dirname "$0")/.." || exit 1
# --fast: без Python (он долгий, только fib у него ~25 секунд)
NOPY=""
[ "$1" = "--fast" ] && NOPY=1
# компилятор на Tachbit собирается из семени, без стартера на C
./tools/build_tb.sh > /dev/null || exit 1
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
    if [ -z "$NOPY" ] && command -v python3 > /dev/null; then
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
if [ -z "$NOPY" ] && command -v python3 > /dev/null; then
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
print(round(r / (1024 * 1024) if sys.platform == 'darwin' else r / 1024, 1))" "$@"
    else
        echo "?"
    fi
}
../tachbit-tb build temp.tb > /dev/null || exit 1
../tachbit-tb build notemp.tb > /dev/null || exit 1
cc -O2 -o temp_c temp_pure.c || exit 1
echo "== temp  (ответ: $(./temp))"
echo "   Tachbit с temp:   $(best ./temp) с, память $(mem ./temp) МБ"
echo "   Tachbit без temp: $(best ./notemp) с, память $(mem ./notemp) МБ  (память шага отдаётся сама)"
echo "   C (malloc/free):  $(best ./temp_c) с, память $(mem ./temp_c) МБ"
if [ -z "$NOPY" ] && command -v python3 > /dev/null; then
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
if [ -z "$NOPY" ] && command -v python3 > /dev/null; then
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

# background: три работы (простые числа на отрезках), две из них в фоне
../tachbit-tb build background.tb > /dev/null || exit 1
../tachbit-tb build nobackground.tb > /dev/null || exit 1
cc -O2 -o background_c background_pure.c || exit 1
echo "== background  (ответ: $(./background))"
echo "   Tachbit background:     $(best ./background) с"
echo "   Tachbit по очереди:     $(best ./nobackground) с"
echo "   C (по очереди):         $(best ./background_c) с"
rm -f background background.c nobackground nobackground.c background_c

# заказ наперёд: data[x] по плану idx, 16 млн случайных номеров, 3 раза
../tachbit-tb build prefetch.tb > /dev/null || exit 1
../tachbit-tb build noprefetch.tb > /dev/null || exit 1
cc -O2 -o prefetch_c prefetch_pure.c || exit 1
echo "== prefetch  (ответ: $(./prefetch))"
echo "   Tachbit с заказом наперёд: $(best ./prefetch) с"
echo "   Tachbit без него:          $(best ./noprefetch) с"
echo "   C (обычный цикл):          $(best ./prefetch_c) с"
rm -f prefetch prefetch.c noprefetch noprefetch.c prefetch_c

# горячие и холодные качества: 5 млн obj, сумма balance 20 раз
../tachbit-tb build hotcold.tb > /dev/null || exit 1
../tachbit-tb build nohotcold.tb > /dev/null || exit 1
cc -O2 -o hotcold_c hotcold_pure.c || exit 1
echo "== hotcold  (ответ: $(./hotcold))"
echo "   Tachbit (холодное отдельно): $(best ./hotcold) с"
echo "   Tachbit (всё вместе):        $(best ./nohotcold) с"
echo "   C (массив структур):         $(best ./hotcold_c) с"
rm -f hotcold hotcold.c nohotcold nohotcold.c hotcold_c

# текст: сколько запятых в тексте на 35 МБ, 10 раз (вместе с созданием текста)
../tachbit-tb build text.tb > /dev/null || exit 1
cc -O2 -o text_c text_pure.c || exit 1
echo "== text  (ответ: $(./text))"
echo "   Tachbit:  $(best ./text) с"
echo "   C:        $(best ./text_c) с"
rm -f text text.c text_c

# склеивание: отчёт из 2 млн строк через out += ... (дописывание в конец своей памяти)
../tachbit-tb build append.tb > /dev/null || exit 1
cc -O2 -o append_c append_pure.c || exit 1
echo "== append  (ответ: $(./append))"
echo "   Tachbit:  $(best ./append) с"
echo "   C:        $(best ./append_c) с"
rm -f append append.c append_c

# решето битами: list bool в Tachbit хранится битами; C — байтами (как primes_pure.c) и битами вручную
../tachbit-tb build primes.tb > /dev/null || exit 1
cc -O2 -o primes_c primes_pure.c || exit 1
cc -O2 -o primes_bits_c primes_bits_pure.c || exit 1
echo "== list bool  (ответ: $(./primes))"
echo "   Tachbit:          $(best ./primes) с"
echo "   C (байты):        $(best ./primes_c) с"
echo "   C (биты вручную): $(best ./primes_bits_c) с"
rm -f primes primes.c primes_c primes_bits_c

# склеивание на всех ядрах: shared out = "" и out += ... внутри on all cores
../tachbit-tb build append_cores.tb > /dev/null || exit 1
../tachbit-tb build noappend_cores.tb > /dev/null || exit 1
cc -O2 -o append_cores_c append_cores_pure.c || exit 1
echo "== append on all cores  (ответ: $(./append_cores))"
echo "   Tachbit (все ядра):  $(best ./append_cores) с"
echo "   Tachbit (одно ядро): $(best ./noappend_cores) с"
echo "   C (одно ядро):       $(best ./append_cores_c) с"
rm -f append_cores append_cores.c noappend_cores noappend_cores.c append_cores_c

# память цикла без temp: компилятор сам отдаёт память каждого шага (наружу уходит только число)
../tachbit-tb build automem.tb > /dev/null || exit 1
cc -O2 -o automem_c automem_pure.c || exit 1
echo "== automem  (ответ: $(./automem))"
echo "   Tachbit:          $(best ./automem) с, память $(mem ./automem) МБ"
echo "   C (malloc/free):  $(best ./automem_c) с, память $(mem ./automem_c) МБ"
rm -f automem automem.c automem_c

# арена функции: функцию зовут 3 млн раз из цикла, который сохраняет наружу (у шага арены нет)
../tachbit-tb build funcmem.tb > /dev/null || exit 1
cc -O2 -o funcmem_c funcmem_pure.c || exit 1
echo "== funcmem  (ответ: $(./funcmem))"
echo "   Tachbit:          $(best ./funcmem) с, память $(mem ./funcmem) МБ"
echo "   C (malloc/free):  $(best ./funcmem_c) с, память $(mem ./funcmem_c) МБ"
rm -f funcmem funcmem.c funcmem_c
