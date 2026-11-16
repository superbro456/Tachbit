#!/bin/bash
# Проверка ядер без ОС: каждый tests/bare/*.tb и все tests/ok/*.tb собираются
# командой tachbit bare и запускаются в QEMU; вывод (порт COM1) сравнивается с .out.
#   ./tests/run_bare.sh            проверить
#   ./tests/run_bare.sh --update   записать новые .out для tests/bare
# Нужны qemu-system-i386 и ld.lld (на Mac: brew install qemu lld).
cd "$(dirname "$0")/.." || exit 1
command -v qemu-system-i386 > /dev/null || { echo "нет qemu-system-i386 (на Mac: brew install qemu)"; exit 1; }
[ -x ./tachbit-tb ] || ./tools/build_tb.sh > /dev/null || exit 1
work=$(mktemp -d)
pass=0; fail=0
run_kernel() {
    ./tachbit-tb bare "$1" > "$work/build.txt" 2>&1 || { cat "$work/build.txt"; return 1; }
    # адреса в сообщении об аварии меняются от сборки к сборке — их убираем
    timeout 60 qemu-system-i386 -no-reboot -kernel "${1%.tb}.elf" -serial stdio -display none \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 2> /dev/null | tr -d '\r' | sed -E 's/ \(0x[0-9a-f]+, 0x[0-9a-f]+\)$//'
    rm -f "${1%.tb}.elf" "${1%.tb}.c"
}
for f in tests/bare/*.tb; do
    got=$(run_kernel "$f")
    if [ "$1" = "--update" ]; then printf '%s\n' "$got" > "${f%.tb}.out"; continue; fi
    if [ "$got" = "$(cat "${f%.tb}.out")" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "не прошёл: $f"; fi
done
# обычные тесты тоже должны работать как ядра (кроме тех, где нужны файлы — их без ОС нет)
for f in tests/ok/*.tb; do
    [ "$(basename "$f")" = "library.tb" ] && continue
    cp "$f" "$work/"
    got=$(run_kernel "$work/$(basename "$f")")
    if [ "$got" = "$(cat "${f%.tb}.out")" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "не прошёл (как ядро): $f"; fi
done
rm -rf "$work"
echo "ядра: прошло $pass, не прошло $fail"
[ $fail -eq 0 ]
