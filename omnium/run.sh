#!/bin/bash
# Собрать Омниум и запустить в QEMU.  ./omnium/run.sh  (окно)   ./omnium/run.sh --text  (вывод в терминал)
cd "$(dirname "$0")/.." || exit 1
# компилятор собираем заново: после git pull старый ./build/tb мог остаться
./tools/build_tb.sh > /dev/null || exit 1
./build/tb bare omnium/main.tb || exit 1
if [ "$1" = "--text" ]; then
    qemu-system-x86_64 -no-reboot -kernel omnium/main.elf -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04
else
    qemu-system-x86_64 -no-reboot -kernel omnium/main.elf -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=0x04
fi
