#!/bin/sh
# Собрать программу Tachbit для FuturaOS (ОС Алексея на базе ToaruOS).
#   tools/futura/build.sh file.tb [выход]
# Нужно: tb, clang, ld.lld и папка с libc.so, libm.so, crt0.o, crti.o, crtn.o из его ramdisk:
#   python3 tools/futura/ramdisk.py sysroot ramdisk.igz ~/.tachbit/futura
# (или своя папка в TB_FUTURA_SYSROOT)
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$1
[ -n "$src" ] || { echo "как: $0 file.tb [выход]"; exit 2; }
out=${2:-${src%.tb}.futura}
root=${TB_FUTURA_SYSROOT:-$HOME/.tachbit/futura}
tb=${TB:-tb}
cc=${CLANG:-clang}
ld=${LLD:-ld.lld}
for f in libc.so libm.so crt0.o crti.o crtn.o; do
    [ -f "$root/$f" ] || { echo "нет $root/$f — сначала: python3 $here/ramdisk.py sysroot ramdisk.igz $root"; exit 1; }
done
tmp=${TMPDIR:-/tmp}/tb-futura-$$
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT
"$tb" check "$src"
"$tb" c "$src" > "$tmp/prog.c"
res=$("$cc" -print-resource-dir)
"$cc" --target=x86_64-unknown-elf -O2 -w -nostdinc \
    -isystem "$here/include" -isystem "$here/toaru-include" -isystem "$res/include" \
    -include tb_futura.h -fno-pic -fno-stack-protector \
    -c "$tmp/prog.c" -o "$tmp/prog.o"
# как у его программ: обычный (не PIE) файл с адреса 0x400000, libc.so через /lib/ld.so.
# Статически нельзя: в статической программе его libc не готовит место для _Thread_local.
"$ld" --image-base=0x400000 -z noseparate-code --no-rosegment -z norelro \
    -dynamic-linker /lib/ld.so --hash-style=sysv -rpath /lib \
    -o "$out" "$root/crt0.o" "$root/crti.o" "$tmp/prog.o" -L"$root" -lm -lc "$root/crtn.o"
echo "готово: $out (программа для FuturaOS)"
