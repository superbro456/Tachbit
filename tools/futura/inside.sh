#!/bin/sh
# Tachbit ВНУТРИ FuturaOS: tb + tcc + cc, чтобы `tb run файл.tb` работал прямо в его ОС.
#   tools/futura/inside.sh папка_tinycc папка_выхода
# Нужно: TB_FUTURA_INCLUDE (его include/), TB_FUTURA_SYSROOT (libc.so, crt*.o), clang, ld.lld.
# Исходники tcc: git clone https://github.com/TinyCC/tinycc
# С LLVM (быстрые программы): TB_FUTURA_LLVM=папка с llc, opt, llvm-link, ld.lld для FuturaOS
# (их делает tools/futura/llvm/build.sh). Тогда tb run в FuturaOS идёт через LLVM, tcc — запасной.
# Потом всё из папки_выхода кладётся в ramdisk:
#   cd папка_выхода && python3 ramdisk.py add ramdisk.tar new.igz $(find . -type f | sed 's|^\./||;s|.*|&=&|')
set -e
here=$(cd "$(dirname "$0")" && pwd)
tcc_src=$(cd "$1" && pwd)
out=$2
[ -n "$out" ] || { echo "как: $0 папка_tinycc папка_выхода"; exit 2; }
inc=${TB_FUTURA_INCLUDE:?нужна TB_FUTURA_INCLUDE — папка include/ от Алексея}
root=${TB_FUTURA_SYSROOT:-$HOME/.tachbit/futura}
tb=${TB:-tb}
mkdir -p "$out/bin" "$out/usr/lib/tcc" "$out/usr/include"
out=$(cd "$out" && pwd)
tmp=${TMPDIR:-/tmp}/tb-inside-$$
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT
res=$(clang -print-resource-dir)
cflags="--target=x86_64-unknown-elf -O2 -w -nostdinc -isystem $here/sdk-include -isystem $inc -isystem $res/include -fno-pic -fno-stack-protector"
link() { ld.lld --image-base=0x400000 -z noseparate-code --no-rosegment -z norelro -dynamic-linker /lib/ld.so \
    --hash-style=sysv -rpath /lib -o "$1" "$root/crt0.o" "$root/crti.o" "$2" -L"$root" -lm -lc "$root/crtn.o"; }

# 1. сам tb
TB_FUTURA_INCLUDE=$inc TB_FUTURA_SYSROOT=$root TB=$tb sh "$here/build.sh" "$here/../../compiler/main.tb" "$out/bin/tb"

# 2. tcc: сначала обычный (на этом компьютере) — он даёт tccdefs_.h и libtcc1.a, потом для FuturaOS
(cd "$tcc_src" && { [ -f config.h ] || ./configure > /dev/null; } && make > /dev/null)
(cd "$tcc_src" && clang $cflags -include tb_futura_sdk.h -DONE_SOURCE=1 -DTCC_TARGET_X86_64 -DCONFIG_TCC_PREDEFS=1 \
    -DTCC_VERSION="\"$(cat VERSION)\"" -DCONFIG_TCCDIR='"/usr/lib/tcc"' -DCONFIG_TCC_ELFINTERP='"/lib/ld.so"' \
    -DCONFIG_TCC_CRTPREFIX='"{B}:/lib"' -DCONFIG_TCC_LIBPATHS='"{B}:/lib:/usr/lib"' \
    -DCONFIG_TCC_SYSINCLUDEPATHS='"{B}/include:/usr/include"' -DCONFIG_TCC_SEMLOCK=0 \
    -DCONFIG_TCC_BACKTRACE=0 -DCONFIG_TCC_BCHECK=0 -c tcc.c -o "$tmp/tcc.o")
link "$out/bin/tcc" "$tmp/tcc.o"
cp "$tcc_src/libtcc1.a" "$out/usr/lib/tcc/"
rm -rf "$out/usr/lib/tcc/include" && cp -r "$tcc_src/include" "$out/usr/lib/tcc/include"
# tcc ищет crt1.o, у FuturaOS это crt0.o; -lpthread: потоки в его libc, библиотека пустая
cp "$root/crt0.o" "$out/usr/lib/tcc/crt1.o"
printf '!<arch>\n' > "$out/usr/lib/tcc/libpthread.a"

# 3. /bin/cc = tcc + tb_futura_sdk.h (его зовёт tb run)
clang $cflags -c "$here/cc.c" -o "$tmp/cc.o"
link "$out/bin/cc" "$tmp/cc.o"

# 4. заголовки: его include/ (без C++, ядра, GL) + поправки Tachbit
(cd "$inc" && tar cf - --exclude=c++ --exclude=libxml2 --exclude='GL*' --exclude=EGL --exclude=KHR \
    --exclude=kernel --exclude='*.bak' --exclude='*.backup*' .) | (cd "$out/usr/include" && tar xf -)
cp -r "$here/sdk-include/." "$out/usr/include/"
# 5. LLVM (если есть) + готовая библиотека Tachbit в LLVM (.bc): в FuturaOS нет clang, чтобы её сделать
if [ -n "$TB_FUTURA_LLVM" ]; then
    for t in llc opt llvm-link ld.lld; do cp "$TB_FUTURA_LLVM/$t" "$out/bin/"; done
    mkdir -p "$out/usr/lib/tachbit"
    name=$("$tb" llvm-lib-name)
    rt="$out/usr/lib/tachbit/$name"
    clang --target=x86_64-unknown-elf -O2 -w -nostdinc -isystem "$here/sdk-include" -isystem "$inc" -isystem "$res/include" \
        -fno-stack-protector -include tb_futura_sdk.h -c -emit-llvm -DTB_LL -x c "$here/../../compiler/runtime.h" -o "$rt.bc"
    dis=$(command -v llvm-dis || command -v llvm-dis-18)
    "$dis" "$rt.bc" -o - | grep -E '^target |^attributes #0 ' > "$rt.target"
fi
echo "готово: $out (bin/tb, bin/tcc, bin/cc, usr/lib/tcc, usr/include)"
