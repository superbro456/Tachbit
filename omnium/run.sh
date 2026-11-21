#!/bin/bash
# Собрать Омниум и запустить в QEMU (4 ядра процессора).
#   ./omnium/run.sh          оболочка «Капля» (окно QEMU: экран, мышь, клавиатура)
#   ./omnium/run.sh --text   первый пример (omnium/main.tb): вывод в терминал
cd "$(dirname "$0")/.." || exit 1
# компилятор собираем заново: после git pull старый ./build/tb мог остаться
./tools/build_tb.sh > /dev/null || exit 1
if [ "$1" = "--text" ]; then
    ./build/tb bare omnium/main.tb || exit 1
    qemu-system-x86_64 -smp 4 -m 256 -no-reboot -kernel omnium/main.elf -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04
else
    ./build/tb bare omnium/desktop.tb || exit 1
    # -rtc base=localtime: часы ядра показывают время твоего компьютера
    qemu-system-x86_64 -smp 4 -m 256 -no-reboot -kernel omnium/desktop.elf -vga std -rtc base=localtime -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=0x04
fi
