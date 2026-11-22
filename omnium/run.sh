#!/bin/bash
# Собрать Омниум и запустить в QEMU (4 ядра процессора).
#   ./omnium/run.sh          оболочка «Капля» (окно QEMU: экран, мышь, клавиатура)
#   ./omnium/run.sh --text   первый пример (omnium/main.tb): вывод в терминал
cd "$(dirname "$0")/.." || exit 1
# ускорение: процессор компьютера работает сам (HVF на Mac, KVM на Linux), а не изображается программой —
# в 10–50 раз быстрее. TB_NO_ACCEL=1 — без него (если вдруг с ним не запускается)
accel=()
if [ -z "$TB_NO_ACCEL" ]; then
    if [ "$(uname)" = Darwin ] && [ "$(sysctl -n kern.hv_support 2>/dev/null)" = 1 ]; then accel=(-accel hvf -cpu host)
    elif [ -w /dev/kvm ]; then accel=(-accel kvm -cpu host); fi
fi
# компилятор собираем заново: после git pull старый ./build/tb мог остаться
./tools/build_tb.sh > /dev/null || exit 1
if [ "$1" = "--text" ]; then
    ./build/tb bare omnium/main.tb || exit 1
    qemu-system-x86_64 "${accel[@]}" -smp 4 -m 256 -no-reboot -kernel omnium/main.elf -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04
else
    ./build/tb bare omnium/desktop.tb || exit 1
    # диск Омниума (файловая система OFS3): создаётся один раз, файлы живут между запусками
    [ -f omnium/disk.img ] || python3 -c "open('omnium/disk.img','wb').truncate(64 << 20)"
    drives=(-drive file=omnium/disk.img,format=raw,if=ide,index=0)
    # второй диск exFAT (флешка): ./omnium/run.sh --usb флешка.img
    if [ "$1" = "--usb" ] && [ -n "$2" ]; then drives+=(-drive file="$2",format=raw,if=ide,index=1); fi
    # -rtc base=localtime: часы ядра показывают время твоего компьютера
    qemu-system-x86_64 "${accel[@]}" -smp 4 -m 256 -no-reboot -kernel omnium/desktop.elf -vga std -rtc base=localtime -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=0x04 "${drives[@]}"
fi
