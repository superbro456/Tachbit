#!/bin/bash
# Проверка ядер без ОС: каждый tests/bare/*.tb и все tests/ok/*.tb собираются
# командой tachbit bare и запускаются в QEMU; вывод (порт COM1) сравнивается с .out.
#   ./tests/run_bare.sh            проверить
#   ./tests/run_bare.sh --update   записать новые .out для tests/bare
# Нужны qemu-system-x86_64 и python3 (на Mac: brew install qemu).
export TB_LANG=ru   # эталоны сообщений — на русском
cd "$(dirname "$0")/.." || exit 1
command -v qemu-system-x86_64 > /dev/null || { echo "нет qemu-system-x86_64 (на Mac: brew install qemu)"; exit 1; }
# компилятор собираем заново: после git pull старый ./build/tb мог остаться
./tools/build_tb.sh > /dev/null || exit 1
work=$(mktemp -d)
pass=0; fail=0
run_kernel() {
    ./build/tb bare "$1" > "$work/build.txt" 2>&1 || { cat "$work/build.txt"; return 1; }
    # адреса в сообщении об аварии меняются от сборки к сборке — их убираем
    # на Mac нет команды timeout: ограничение по времени через perl (он есть везде)
    perl -e 'alarm shift; exec @ARGV' 60 qemu-system-x86_64 -smp "${TB_SMP:-1}" -no-reboot -kernel "${1%.tb}.elf" ${TB_INITRD:+-initrd "$TB_INITRD"} -serial stdio -display none \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 2> "$work/qemu.txt" < /dev/null | tr -d '\r' | sed -E 's/ \(0x[0-9a-f]+, 0x[0-9a-f]+\)$//'
    # QEMU ничего не напечатал — покажем, что он сказал сам
    [ -s "$work/qemu.txt" ] && sed 's/^/qemu: /' "$work/qemu.txt" | head -3
    rm -f "${1%.tb}.elf" "${1%.tb}.c"
}
for f in tests/bare/*.tb; do
    got=$(run_kernel "$f")
    if [ "$1" = "--update" ]; then printf '%s\n' "$got" > "${f%.tb}.out"; continue; fi
    if [ "$got" = "$(cat "${f%.tb}.out")" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "не прошёл: $f"; [ $fail -eq 1 ] && printf '%s\n' "$got" | head -5 | sed 's/^/    /'; fi
done
# обычные тесты тоже должны работать как ядра (кроме тех, где нужны файлы — их без ОС нет)
for f in tests/ok/*.tb; do
    case "$(basename "$f")" in library.tb|folders.tb|random_time.tb|json_date.tb|c_call.tb|zone_syscall.tb|net.tb) continue ;; esac
    cp "$f" "$work/"
    got=$(run_kernel "$work/$(basename "$f")")
    if [ "$got" = "$(cat "${f%.tb}.out")" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "не прошёл (как ядро): $f"; fi
done
# Омниум (omnium/main.tb): службы и клетки на каналах; клавиатуру никто не трогает — эхо пустое
got=$(run_kernel omnium/main.tb)
if [ "$got" = "$(cat omnium/main.out)" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "не прошёл: omnium/main.tb"; fi
# файловая служба OFS3 (omnium/ofs.tb): версии, снимки, ключи, отзыв ключа
got=$(run_kernel omnium/ofs_test.tb)
if [ "$got" = "$(cat omnium/ofs_test.out)" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "не прошёл: omnium/ofs_test.tb"; fi
# картинки в ядре: PNG, JPEG, GIF, BMP из файлов и из памяти (omnium/image_test.tb)
got=$(run_kernel omnium/image_test.tb)
if [ "$got" = "$(cat omnium/image_test.out)" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "не прошёл: omnium/image_test.tb"; fi
# программы Омниума: tb app делает .oapp, ядро грузит их из модулей (-initrd) и запускает клетками
# (own.tb — своим машинным кодом: tb app --own, без LLVM)
./build/tb app tests/apps/hello.tb > /dev/null && ./build/tb app tests/apps/crash.tb > /dev/null && ./build/tb app tests/apps/own.tb --own > /dev/null
got=$(TB_INITRD="tests/apps/hello.oapp,tests/apps/crash.oapp,tests/apps/own.oapp" run_kernel omnium/app_test.tb)
rm -f tests/apps/*.oapp
if [ "$got" = "$(cat omnium/app_test.out)" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "не прошёл: omnium/app_test.tb"; printf '%s\n' "$got" | head -8 | sed 's/^/    /'; fi
# OFS3 на диске: несколько запусков на одном образе, сбой суперблока, испорченный сектор
if ./tests/omnium_disk.sh > "$work/disk.txt"; then pass=$((pass + 1)); else fail=$((fail + 1)); cat "$work/disk.txt"; fi
# оболочка «Капля» (omnium/desktop.tb): здесь только сборка — ей нужен живой экран и мышь
if ./build/tb bare omnium/desktop.tb > "$work/build.txt" 2>&1; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "не собралась: omnium/desktop.tb"; cat "$work/build.txt"; fi
rm -f omnium/desktop.elf omnium/desktop.c
# те же проверки ядра на 4 ядрах процессора (задачи идут по-настоящему одновременно:
# строки разных клеток могут прийти в другом порядке — сравниваем сами строки)
for f in tests/bare/*.tb; do
    got=$(TB_SMP=4 run_kernel "$f" | sort)
    if [ "$got" = "$(sort "${f%.tb}.out")" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "не прошёл на 4 ядрах: $f"; fi
done
# Омниум на 4 ядрах: клетки пишут одновременно — порядок строк бывает разный, сами строки те же
got=$(TB_SMP=4 run_kernel omnium/main.tb | sort)
if [ "$got" = "$(sort omnium/main.out)" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "не прошёл на 4 ядрах: omnium/main.tb"; fi
rm -rf "$work"
echo "ядра: прошло $pass, не прошло $fail"
[ $fail -eq 0 ]
