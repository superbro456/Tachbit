#!/bin/bash
# Делает установщик Tachbit для Windows: build/windows/Tachbit-Setup-<версия>.exe
# Нужны (Linux): x86_64-w64-mingw32-gcc (пакет mingw-w64) и makensis (пакет nsis).
#   sudo apt install mingw-w64 nsis
#   ./tools/windows/make_installer.sh
set -e
cd "$(dirname "$0")/../.."
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "нужен x86_64-w64-mingw32-gcc (apt install mingw-w64)"; exit 1; }
command -v makensis >/dev/null || { echo "нужен makensis (apt install nsis)"; exit 1; }

[ -x ./tachbit-tb ] || ./tools/build_tb.sh
version=$(./tachbit-tb version | sed -n 's/^Tachbit \([0-9.]*\).*/\1/p')
out=build/windows
rm -rf "$out/stage"
mkdir -p "$out/stage"

# 1) компилятор Tachbit как C, потом tb.exe для Windows (без чужих .dll, стек 8 МБ)
./tachbit-tb c compiler/main.tb > "$out/tb.c"
x86_64-w64-mingw32-gcc -O2 -w -static -Wl,--stack,8388608 -o "$out/stage/tb.exe" "$out/tb.c" -lm -lpthread
x86_64-w64-mingw32-strip "$out/stage/tb.exe"

# 2) что ещё кладём в установщик
cp tools/windows/installer.nsi tools/windows/setup-helper.ps1 tools/windows/tachbit.ico "$out/stage/"
cp LICENSE "$out/stage/LICENSE.txt"
cp README.md README.en.md "$out/stage/"
cp -r examples "$out/stage/examples"

# 3) установщик
(cd "$out/stage" && makensis -V2 -DVERSION="$version" -DOUTFILE="../Tachbit-Setup-$version.exe" installer.nsi)
rm -f "$out/tb.c"
echo "готово: $out/Tachbit-Setup-$version.exe"
