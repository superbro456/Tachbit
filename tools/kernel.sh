#!/bin/bash
# Собрать ядро без ОС и запустить его в эмуляторе QEMU.
#   ./tools/kernel.sh                         — пример examples/kernel/main.tb в окне
#   ./tools/kernel.sh файл.tb                 — своё ядро в окне
#   ./tools/kernel.sh файл.tb --text          — без окна: вывод ядра прямо в терминал
# На Mac один раз:  brew install qemu   (компоновщик свой, на Python; ld.lld не нужен)
cd "$(dirname "$0")/.." || exit 1
# компилятор собираем заново: после git pull старый ./build/tb мог остаться
./tools/build_tb.sh > /dev/null || exit 1
src=${1:-examples/kernel/main.tb}
./build/tb bare "$src" > /dev/null || exit 1
elf="${src%.tb}.elf"
if [ "$2" = "--text" ]; then
    # ядро пишет и в COM1 → терминал; exit(0) в ядре закрывает QEMU (код 1 = всё хорошо)
    qemu-system-x86_64 -no-reboot -kernel "$elf" -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04
    [ $? -eq 1 ] && exit 0 || exit 1
fi
echo "окно QEMU: закрой его, когда насмотришься"
qemu-system-x86_64 -no-reboot -kernel "$elf"
