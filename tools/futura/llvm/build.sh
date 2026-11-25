#!/bin/sh
# LLVM для FuturaOS: llc, opt, llvm-link и ld.lld, которые работают ВНУТРИ его ОС.
# Тогда `tb run` там идёт через LLVM, как на Linux и Mac (без clang).
#
#   tools/futura/llvm/build.sh llvm-project-18.1.8.src папка_работы папка_выхода
#
# Нужно: TB_FUTURA_INCLUDE (его include/), TB_FUTURA_SYSROOT (libc.so, libm.so, crt*.o),
# clang 18, ld.lld, cmake, ninja, исходники LLVM 18:
#   https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/llvm-project-18.1.8.src.tar.xz
# Идёт около полутора часов на 4 ядрах. Шаги:
#   1. llvm-tblgen для этого компьютера;
#   2. sysroot: его заголовки + поправки (sysexits.h, nl_item, restrict в spawn.h);
#   3. libc++, libc++abi, libunwind (LLVM) — статически; его собственный libc++ другой (не LLVM);
#   4. compiler-rt builtins;
#   5. сами инструменты LLVM + lld, со свёрткой mmap/mprotect (fut_compat.c).
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$1" && pwd)
work=$2
out=$3
[ -n "$out" ] || { echo "как: $0 llvm-project-src папка_работы папка_выхода"; exit 2; }
inc=${TB_FUTURA_INCLUDE:?нужна TB_FUTURA_INCLUDE — папка include/ от Алексея}
root=${TB_FUTURA_SYSROOT:-$HOME/.tachbit/futura}
mkdir -p "$work" "$out/bin"
work=$(cd "$work" && pwd)
out=$(cd "$out" && pwd)
S=$work/sysroot
res=$(clang -print-resource-dir)

# 1
mkdir -p "$work/host"
(cd "$work/host" && cmake -G Ninja "$src/llvm" -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=X86 \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF > /dev/null && ninja llvm-tblgen)

# 2
mkdir -p "$S/usr/include" "$S/usr/lib"
(cd "$inc" && tar cf - --exclude=c++ --exclude=libxml2 --exclude='GL*' --exclude=EGL --exclude=KHR \
    --exclude='*.bak' --exclude='*.backup*' .) | (cd "$S/usr/include" && tar xf -)
printf '#pragma once\n/* FuturaOS: нет sysexits.h */\n#define EX_OK 0\n#define EX_USAGE 64\n#define EX_SOFTWARE 70\n#define EX_IOERR 74\n' > "$S/usr/include/sysexits.h"
sed -i 's/typedef long nl_item;/typedef int nl_item;/' "$S/usr/include/nl_types.h"
sed -i 's/argv\[restrict\]/argv[]/; s/envp\[restrict\]/envp[]/' "$S/usr/include/spawn.h"
sed -i 's/argv\[restrict\]/argv[]/; s/envp\[restrict\]/envp[]/' "$S/usr/include/spawn.h"
for f in libc.so libm.so crt0.o crti.o crtn.o; do cp "$root/$f" "$S/usr/lib/"; done
cp "$root/crt0.o" "$S/usr/lib/crt1.o"
for l in pthread pthreads dl rt; do printf '!<arch>\n' > "$S/usr/lib/lib$l.a"; done
sed "s|@SYSROOT@|$S|g; s|@HERE@|$here|g" "$here/futura.cmake.in" > "$work/futura.cmake"
clang --target=x86_64-unknown-elf -O2 -nostdinc -isystem "$S/usr/include" -isystem "$res/include" -fno-pic \
    -c "$here/fut_compat.c" -o "$S/usr/lib/fut_compat.o"
# libc++: у FuturaOS есть newlocale, но нет *_l функций — берём запасные из самого libc++
python3 - "$src/libcxx/include/__locale" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
if "__futura__" not in s:
    s = s.replace("#elif defined(_NEWLIB_VERSION)\n#  include <__support/newlib/xlocale.h>",
        "#elif defined(__futura__)\n#  include <__support/xlocale/__posix_l_fallback.h>\n#  include <__support/xlocale/__strtonum_fallback.h>\n"
        "#elif defined(_NEWLIB_VERSION)\n#  include <__support/newlib/xlocale.h>", 1)
    open(p, "w").write(s)
PY

# 3
mkdir -p "$work/rt"
(cd "$work/rt" && cmake -G Ninja "$src/runtimes" -DCMAKE_TOOLCHAIN_FILE="$work/futura.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" -DCMAKE_INSTALL_PREFIX="$S/usr" \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_SHARED=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF \
    -DLIBCXX_HAS_PTHREAD_API=ON -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
    -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF \
    -DLIBUNWIND_ENABLE_WERROR=OFF > /dev/null && ninja cxx cxxabi unwind && ninja install-cxx install-cxxabi install-unwind > /dev/null)

# 4
mkdir -p "$work/crt"
(cd "$work/crt" && cmake -G Ninja "$src/compiler-rt/lib/builtins" -DCMAKE_TOOLCHAIN_FILE="$work/futura.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_BAREMETAL_BUILD=ON \
    -DLLVM_CMAKE_DIR="$src/llvm/cmake/modules" > /dev/null && ninja)
cp "$work/crt/lib/linux/libclang_rt.builtins-x86_64.a" "$S/usr/lib/libclang_rt.builtins.a"

# 5
mkdir -p "$work/fut"
(cd "$work/fut" && cmake -G Ninja "$src/llvm" -DCMAKE_TOOLCHAIN_FILE="$work/futura.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS=lld -DLLVM_TABLEGEN="$work/host/bin/llvm-tblgen" \
    -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-elf -DLLVM_HOST_TRIPLE=x86_64-unknown-linux-gnu \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_PIC=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_BACKTRACES=OFF -DLLVM_ENABLE_CRASH_OVERRIDES=OFF \
    -DLLVM_ENABLE_PLUGINS=OFF -DLLVM_ENABLE_LIBCXX=ON -DHAVE_BACKTRACE=OFF -DHAVE_POSIX_FALLOCATE=0 \
    -DHAVE_GETRLIMIT=0 -DHAVE_SETRLIMIT=0 -DHAVE_SYSEXITS_H=1 > /dev/null && ninja llc opt llvm-link lld)
strip=$(command -v llvm-strip || command -v llvm-strip-18)
for t in llc opt llvm-link; do "$strip" -o "$out/bin/$t" "$work/fut/bin/$t"; done
"$strip" -o "$out/bin/ld.lld" "$work/fut/bin/lld"
echo "готово: $out/bin (llc, opt, llvm-link, ld.lld для FuturaOS)"
